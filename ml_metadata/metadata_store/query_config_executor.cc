/* Copyright 2019 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "ml_metadata/metadata_store/query_config_executor.h"

#include <string>
#include <vector>

#include "google/protobuf/struct.pb.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/text_format.h"
#include "google/protobuf/util/json_util.h"
#include "absl/status/status.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "ml_metadata/metadata_store/list_operation_query_helper.h"
#include "ml_metadata/proto/metadata_source.pb.h"
#include "ml_metadata/proto/metadata_store.pb.h"
#ifndef _WIN32
#include "ml_metadata/query/filter_query_ast_resolver.h"
#include "ml_metadata/query/filter_query_builder.h"
#endif
#include "ml_metadata/util/return_utils.h"
#include "ml_metadata/util/struct_utils.h"

namespace ml_metadata {

QueryConfigExecutor::QueryConfigExecutor(
    const MetadataSourceQueryConfig& query_config, MetadataSource* source,
    int64 query_version)
    : QueryExecutor(query_version),
      query_config_(query_config),
      metadata_source_(source) {}

absl::Status QueryConfigExecutor::CheckParentTypeTable() {
  return ExecuteQuery(query_config_.check_parent_type_table());
}

absl::Status QueryConfigExecutor::InsertParentType(int64 type_id,
                                                   int64 parent_type_id) {
  return ExecuteQuery(query_config_.insert_parent_type(),
                      {Bind(type_id), Bind(parent_type_id)});
}

absl::Status QueryConfigExecutor::DeleteParentType(int64 type_id,
                                                   int64 parent_type_id) {
  return ExecuteQuery(query_config_.delete_parent_type(),
                      {Bind(type_id), Bind(parent_type_id)});
}

absl::Status QueryConfigExecutor::SelectParentTypesByTypeID(
    absl::Span<const int64> type_ids, RecordSet* record_set) {
  return ExecuteQuery(query_config_.select_parent_type_by_type_id(),
                      {Bind(type_ids)}, record_set);
}

absl::Status QueryConfigExecutor::InsertEventPath(
    int64 event_id, const Event::Path::Step& step) {
  // Inserts a path into the EventPath table. It has 4 parameters
  // $0 is the event_id
  // $1 is the step value case, either index or key
  // $2 is the is_index_step indicates the step value case
  // $3 is the value of the step
  if (step.has_index()) {
    return ExecuteQuery(
        query_config_.insert_event_path(),
        {Bind(event_id), "step_index", Bind(true), Bind(step.index())});
  } else if (step.has_key()) {
    return ExecuteQuery(
        query_config_.insert_event_path(),
        {Bind(event_id), "step_key", Bind(false), Bind(step.key())});
  }
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::CheckParentContextTable() {
  return ExecuteQuery(query_config_.check_parent_context_table());
}

absl::Status QueryConfigExecutor::InsertParentContext(int64 parent_id,
                                                      int64 child_id) {
  return ExecuteQuery(query_config_.insert_parent_context(),
                      {Bind(child_id), Bind(parent_id)});
}

absl::Status QueryConfigExecutor::SelectParentContextsByContextID(
    int64 context_id, RecordSet* record_set) {
  return ExecuteQuery(query_config_.select_parent_context_by_context_id(),
                      {Bind(context_id)}, record_set);
}

absl::Status QueryConfigExecutor::SelectChildContextsByContextID(
    int64 context_id, RecordSet* record_set) {
  return ExecuteQuery(
      query_config_.select_parent_context_by_parent_context_id(),
      {Bind(context_id)}, record_set);
}

absl::Status QueryConfigExecutor::GetSchemaVersion(int64* db_version) {
  RecordSet record_set;
  absl::Status maybe_schema_version_status =
      ExecuteQuery(query_config_.check_mlmd_env_table(), {}, &record_set);
  if (maybe_schema_version_status.ok()) {
    if (record_set.records_size() == 0) {
      return absl::AbortedError(
          "In the given db, MLMDEnv table exists but no schema_version can be "
          "found. This may be due to concurrent connection to the empty "
          "database. Please retry connection.");

    } else if (record_set.records_size() > 1) {
      return absl::DataLossError(absl::StrCat(
          "In the given db, MLMDEnv table exists but schema_version cannot be "
          "resolved due to there being more than one rows with the schema "
          "version. Expecting a single row: ",
          record_set.DebugString()));
    }
    CHECK(absl::SimpleAtoi(record_set.records(0).values(0), db_version));
    return absl::OkStatus();
  }
  // if MLMDEnv does not exist, it may be the v0.13.2 release or an empty db.
  absl::Status maybe_v0_13_2_status =
      ExecuteQuery(query_config_.check_tables_in_v0_13_2(), {}, &record_set);
  if (maybe_v0_13_2_status.ok()) {
    *db_version = 0;
    return absl::OkStatus();
  }
  return absl::NotFoundError("it looks an empty db is given.");
}

absl::Status QueryConfigExecutor::UpgradeMetadataSourceIfOutOfDate(
    bool enable_migration) {
  int64 db_version = 0;
  absl::Status get_schema_version_status = GetSchemaVersion(&db_version);
  int64 lib_version = GetLibraryVersion();
  if (absl::IsNotFound(get_schema_version_status)) {
    db_version = lib_version;
  } else {
    MLMD_RETURN_IF_ERROR(get_schema_version_status);
  }

  bool is_compatible = false;
  MLMD_RETURN_IF_ERROR(IsCompatible(db_version, lib_version, &is_compatible));
  if (is_compatible) {
    return absl::OkStatus();
  }
  if (db_version > lib_version) {
    return absl::FailedPreconditionError(absl::StrCat(
        "MLMD database version ", db_version,
        " is greater than library version ", lib_version,
        ". Please upgrade the library to use the given database in order to "
        "prevent potential data loss. If data loss is acceptable, please"
        " downgrade the database using a newer version of library."));
  }
  // returns error if upgrade is explicitly disabled, as we are missing schema
  // and cannot continue with this library version.
  if (db_version < lib_version && !enable_migration) {
    return absl::FailedPreconditionError(absl::StrCat(
        "MLMD database version ", db_version, " is older than library version ",
        lib_version,
        ". Schema migration is disabled. Please upgrade the database then use"
        " the library version; or switch to a older library version to use the"
        " current database. For more details, please refer to ml-metadata"
        " g3doc/third_party/ml_metadata/g3doc/"
        "get_started.md#upgrade-the-database-schema"));
  }

  // migrate db_version to lib version
  const auto& migration_schemes = query_config_.migration_schemes();
  while (db_version < lib_version) {
    const int64 to_version = db_version + 1;
    if (migration_schemes.find(to_version) == migration_schemes.end()) {
      return absl::InternalError(absl::StrCat(
          "Cannot find migration_schemes to version ", to_version));
    }
    for (const MetadataSourceQueryConfig::TemplateQuery& upgrade_query :
         migration_schemes.at(to_version).upgrade_queries()) {
      MLMD_RETURN_WITH_CONTEXT_IF_ERROR(
          ExecuteQuery(upgrade_query.query()),
          absl::StrCat("Upgrade query failed: ", upgrade_query.query()));
    }
    MLMD_RETURN_WITH_CONTEXT_IF_ERROR(UpdateSchemaVersion(to_version),
                                      "Failed to update schema.");
    db_version = to_version;
  }
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::SelectLastInsertID(int64* last_insert_id) {
  RecordSet record_set;
  MLMD_RETURN_IF_ERROR(
      ExecuteQuery(query_config_.select_last_insert_id(), {}, &record_set));
  if (record_set.records_size() == 0) {
    return absl::InternalError("Could not find last insert ID: no record");
  }
  const RecordSet::Record& record = record_set.records(0);
  if (record.values_size() == 0) {
    return absl::InternalError("Could not find last insert ID: missing value");
  }
  if (!absl::SimpleAtoi(record.values(0), last_insert_id)) {
    return absl::InternalError("Could not parse last insert ID as string");
  }
  return absl::OkStatus();
}

absl::Status ml_metadata::QueryConfigExecutor::CheckTablesIn_V0_13_2() {
  return ExecuteQuery(query_config_.check_tables_in_v0_13_2());
}

absl::Status QueryConfigExecutor::DowngradeMetadataSource(
    const int64 to_schema_version) {
  const int64 lib_version = query_config_.schema_version();
  if (to_schema_version < 0 || to_schema_version > lib_version) {
    return absl::InvalidArgumentError(absl::StrCat(
        "MLMD cannot be downgraded to schema_version: ", to_schema_version,
        ". The target version should be greater or equal to 0, and the current"
        " library version: ",
        lib_version, " needs to be greater than the target version."));
  }
  int64 db_version = 0;
  absl::Status get_schema_version_status = GetSchemaVersion(&db_version);
  // if it is an empty database, then we skip downgrade and returns.
  if (absl::IsNotFound(get_schema_version_status)) {
    return absl::InvalidArgumentError(
        "Empty database is given. Downgrade operation is not needed.");
  }
  MLMD_RETURN_IF_ERROR(get_schema_version_status);
  if (db_version > lib_version) {
    return absl::FailedPreconditionError(
        absl::StrCat("MLMD database version ", db_version,
                     " is greater than library version ", lib_version,
                     ". The current library does not know how to downgrade it. "
                     "Please upgrade the library to downgrade the schema."));
  }
  // perform downgrade
  const auto& migration_schemes = query_config_.migration_schemes();
  while (db_version > to_schema_version) {
    const int64 to_version = db_version - 1;
    if (migration_schemes.find(to_version) == migration_schemes.end()) {
      return absl::InternalError(absl::StrCat(
          "Cannot find migration_schemes to version ", to_version));
    }
    for (const MetadataSourceQueryConfig::TemplateQuery& downgrade_query :
         migration_schemes.at(to_version).downgrade_queries()) {
      MLMD_RETURN_WITH_CONTEXT_IF_ERROR(ExecuteQuery(downgrade_query),
                                        "Failed to migrate existing db; the "
                                        "migration transaction rolls back.");
    }
    // at version 0, v0.13.2, there is no schema version information.
    if (to_version > 0) {
      MLMD_RETURN_WITH_CONTEXT_IF_ERROR(
          UpdateSchemaVersion(to_version),
          "Failed to migrate existing db; the migration transaction rolls "
          "back.");
    }
    db_version = to_version;
  }
  return absl::OkStatus();
}

std::string QueryConfigExecutor::Bind(const char* value) {
  return absl::StrCat("'", metadata_source_->EscapeString(value), "'");
}

std::string QueryConfigExecutor::Bind(absl::string_view value) {
  return absl::StrCat("'", metadata_source_->EscapeString(value), "'");
}

std::string QueryConfigExecutor::Bind(int value) {
  return std::to_string(value);
}

std::string QueryConfigExecutor::Bind(int64 value) {
  return std::to_string(value);
}

std::string QueryConfigExecutor::Bind(double value) {
  return std::to_string(value);
}

std::string QueryConfigExecutor::Bind(const google::protobuf::Any& value) {
  return absl::StrCat(
      "'",
      metadata_source_->EscapeString(
          metadata_source_->EncodeBytes(value.SerializeAsString())),
      "'");
}

std::string QueryConfigExecutor::Bind(bool value) { return value ? "1" : "0"; }

// Utility method to bind an Event::Type enum value to a SQL clause.
// Event::Type is an enum (integer), EscapeString is not applicable.
std::string QueryConfigExecutor::Bind(const Event::Type value) {
  return std::to_string(value);
}

std::string QueryConfigExecutor::Bind(PropertyType value) {
  return std::to_string((int)value);
}

std::string QueryConfigExecutor::Bind(TypeKind value) {
  return std::to_string((int)value);
}

std::string QueryConfigExecutor::Bind(Artifact::State value) {
  return std::to_string((int)value);
}

std::string QueryConfigExecutor::Bind(Execution::State value) {
  return std::to_string((int)value);
}

std::string QueryConfigExecutor::Bind(absl::Span<const int64> value) {
  return absl::StrJoin(value, ", ");
}

std::string QueryConfigExecutor::Bind(absl::Span<absl::string_view> value) {
  std::vector<std::string> escape_string_value;
  escape_string_value.reserve(value.size());
  for (const auto& v : value) {
    escape_string_value.push_back(Bind(v));
  }
  return absl::StrJoin(escape_string_value, ", ");
}

std::string QueryConfigExecutor::Bind(
    absl::Span<std::pair<absl::string_view, absl::string_view>> value) {
  std::vector<std::string> escape_string_value;
  escape_string_value.reserve(value.size());
  for (const auto& v : value) {
    escape_string_value.push_back(
        absl::StrCat("(", Bind(v.first), ",", Bind(v.second), ")"));
  }
  return absl::StrJoin(escape_string_value, ", ");
}

std::string QueryConfigExecutor::BindValue(const Value& value) {
  switch (value.value_case()) {
    case PropertyType::INT:
      return Bind(value.int_value());
    case PropertyType::DOUBLE:
      return Bind(value.double_value());
    case PropertyType::STRING:
      return Bind(value.string_value());
    case PropertyType::STRUCT:
      return Bind(StructToString(value.struct_value()));
    case PropertyType::PROTO:
      return Bind(value.proto_value());
    case PropertyType::BOOLEAN:
      return Bind(value.bool_value());
    default:
      LOG(FATAL) << "Unknown registered property type: " << value.value_case()
                 << "This is an internal error: properties should have been "
                    "checked before"
                    " they got here";
  }
}

std::string QueryConfigExecutor::BindDataType(const Value& value) {
  switch (value.value_case()) {
    case PropertyType::INT: {
      return "int_value";
      break;
    }
    case PropertyType::DOUBLE: {
      return "double_value";
      break;
    }
    case PropertyType::STRING:
    case PropertyType::STRUCT: {
      return "string_value";
      break;
    }
    case PropertyType::PROTO: {
      return "proto_value";
      break;
    }
    case PropertyType::BOOLEAN: {
      return "bool_value";
      break;
    }
    default: {
      LOG(FATAL) << "Unexpected oneof: " << value.DebugString();
    }
  }
}

std::string QueryConfigExecutor::Bind(const ArtifactStructType* message) {
  if (message) {
    std::string json_output;
    CHECK(::google::protobuf::util::MessageToJsonString(*message, &json_output).ok())
        << "Could not write proto to JSON: " << message->DebugString();
    return Bind(json_output);
  } else {
    return "null";
  }
}

#if (!defined(__APPLE__) && !defined(_WIN32))
string QueryConfigExecutor::Bind(
    const google::protobuf::int64 value) {
  return std::to_string(value);
}
#endif

absl::Status QueryConfigExecutor::ExecuteQuery(const std::string& query) {
  RecordSet record_set;
  return metadata_source_->ExecuteQuery(query, &record_set);
}

absl::Status QueryConfigExecutor::ExecuteQuery(const std::string& query,
                                               RecordSet* record_set) {
  return metadata_source_->ExecuteQuery(query, record_set);
}

absl::Status QueryConfigExecutor::ExecuteQuery(
    const MetadataSourceQueryConfig::TemplateQuery& template_query,
    absl::Span<const std::string> parameters, RecordSet* record_set) {
  if (parameters.size() > 10) {
    return absl::InvalidArgumentError(
        "Template query has too many parameters (at most 10 is supported).");
  }
  if (template_query.parameter_num() != parameters.size()) {
    LOG(FATAL) << "Template query parameter_num ("
               << template_query.parameter_num()
               << ") does not match with given "
               << "parameters size (" << parameters.size()
               << "): " << template_query.DebugString();
  }
  std::vector<std::pair<const std::string, const std::string>> replacements;
  replacements.reserve(parameters.size());
  for (int i = 0; i < parameters.size(); i++) {
    replacements.push_back({absl::StrCat("$", i), parameters[i]});
  }
  return metadata_source_->ExecuteQuery(
      absl::StrReplaceAll(template_query.query(), replacements), record_set);
}

absl::Status QueryConfigExecutor::IsCompatible(int64 db_version,
                                               int64 lib_version,
                                               bool* is_compatible) {
  // Currently, we don't support a database version that is older than the
  // library version. Revisit this if a more sophisticated rule is required.
  *is_compatible = (db_version == lib_version);
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::InitMetadataSource() {
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.create_type_table()));
  MLMD_RETURN_IF_ERROR(
      ExecuteQuery(query_config_.create_type_property_table()));
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.create_parent_type_table()));
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.create_artifact_table()));
  MLMD_RETURN_IF_ERROR(
      ExecuteQuery(query_config_.create_artifact_property_table()));
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.create_execution_table()));
  MLMD_RETURN_IF_ERROR(
      ExecuteQuery(query_config_.create_execution_property_table()));
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.create_event_table()));
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.create_event_path_table()));
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.create_mlmd_env_table()));
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.create_context_table()));
  MLMD_RETURN_IF_ERROR(
      ExecuteQuery(query_config_.create_context_property_table()));
  MLMD_RETURN_IF_ERROR(
      ExecuteQuery(query_config_.create_parent_context_table()));
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.create_association_table()));
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.create_attribution_table()));
  for (const MetadataSourceQueryConfig::TemplateQuery& index_query :
       query_config_.secondary_indices()) {
    const absl::Status status = ExecuteQuery(index_query);
    // For databases (e.g., MySQL), idempotency of indexing creation is not
    // supported well. We handle it here and covered by the `InitForReset` test.
    if (!status.ok() && absl::StrContains(std::string(status.message()),
                                          "Duplicate key name")) {
      continue;
    }
    MLMD_RETURN_IF_ERROR(status);
  }

  int64 library_version = GetLibraryVersion();
  absl::Status insert_schema_version_status =
      InsertSchemaVersion(library_version);
  if (!insert_schema_version_status.ok()) {
    int64 db_version = -1;
    MLMD_RETURN_IF_ERROR(GetSchemaVersion(&db_version));
    if (db_version != library_version) {
      return absl::DataLossError(absl::StrCat(
          "The database cannot be initialized with the schema_version in the "
          "current library. Current library version: ",
          library_version, ", the db version on record is: ", db_version,
          ". It may result from a data race condition caused by other "
          "concurrent MLMD's migration procedures."));
    }
  }
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::InitMetadataSourceIfNotExists(
    const bool enable_upgrade_migration) {
  // If |query_schema_version_| is given, then the query executor is expected to
  // work with an existing db with an earlier schema version equals to that.
  if (query_schema_version()) {
    return CheckSchemaVersionAlignsWithQueryVersion();
  }
  // When working at head, we reuse existing db or create a new db.
  // check db version, and make it to align with the lib version.
  MLMD_RETURN_IF_ERROR(
      UpgradeMetadataSourceIfOutOfDate(enable_upgrade_migration));
  // if lib and db versions align, we check the required tables for the lib.
  std::vector<std::pair<absl::Status, std::string>> checks;
  checks.push_back({CheckTypeTable(), "type_table"});
  checks.push_back({CheckParentTypeTable(), "parent_type_table"});
  checks.push_back({CheckTypePropertyTable(), "type_property_table"});
  checks.push_back({CheckArtifactTable(), "artifact_table"});
  checks.push_back({CheckArtifactPropertyTable(), "artifact_property_table"});
  checks.push_back({CheckExecutionTable(), "execution_table"});
  checks.push_back({CheckExecutionPropertyTable(), "execution_property_table"});
  checks.push_back({CheckEventTable(), "event_table"});
  checks.push_back({CheckEventPathTable(), "event_path_table"});
  checks.push_back({CheckMLMDEnvTable(), "mlmd_env_table"});
  checks.push_back({CheckContextTable(), "context_table"});
  checks.push_back({CheckParentContextTable(), "parent_context_table"});
  checks.push_back({CheckContextPropertyTable(), "context_property_table"});
  checks.push_back({CheckAssociationTable(), "association_table"});
  checks.push_back({CheckAttributionTable(), "attribution_table"});
  std::vector<std::string> missing_schema_error_messages;
  std::vector<std::string> successful_checks;
  std::vector<std::string> failing_checks;
  for (const auto& check_pair : checks) {
    const absl::Status& check = check_pair.first;
    const std::string& name = check_pair.second;
    if (!check.ok()) {
      missing_schema_error_messages.push_back(check.ToString());
      failing_checks.push_back(name);
    } else {
      successful_checks.push_back(name);
    }
  }

  // all table required by the current lib version exists
  if (missing_schema_error_messages.empty()) return absl::OkStatus();

  // some table exists, but not all.
  if (checks.size() != missing_schema_error_messages.size()) {
    return absl::AbortedError(absl::StrCat(
        "There are a subset of tables in MLMD instance. This may be due to "
        "concurrent connection to the empty database. "
        "Please retry the connection. checks: ",
        checks.size(), " errors: ", missing_schema_error_messages.size(),
        ", present tables: ", absl::StrJoin(successful_checks, ", "),
        ", missing tables: ", absl::StrJoin(failing_checks, ", "),
        " Errors: ", absl::StrJoin(missing_schema_error_messages, "\n")));
  }

  // no table exists, then init the MetadataSource
  return InitMetadataSource();
}

absl::Status QueryConfigExecutor::InsertArtifactType(
    const std::string& name, absl::optional<absl::string_view> version,
    absl::optional<absl::string_view> description,
    absl::optional<absl::string_view> external_id, int64* type_id) {
  // TODO(b/248836219): Cleanup the fat-client after fully migrated to V9+.
  if (query_schema_version().has_value() &&
      query_schema_version().value() < kSchemaVersionNine) {
    MetadataSourceQueryConfig::TemplateQuery insert_artifact_type;
    MLMD_RETURN_IF_ERROR(GetTemplateQueryOrDie(
        entity_query::v7_and_v8::kInsertArtifactType, insert_artifact_type));
    return ExecuteQuerySelectLastInsertID(
        insert_artifact_type, {Bind(name), Bind(version), Bind(description)},
        type_id);
  }

  if (external_id.has_value()) {
    RecordSet record;
    MLMD_RETURN_IF_ERROR(
        ExecuteQuery(query_config_.select_types_by_external_ids(),
                     {Bind({external_id.value()}), Bind(1)}, &record));
    if (record.records_size() > 0) {
      return absl::AlreadyExistsError(absl::StrCat(
          "Conflict of external_id: ", external_id.value(),
          " Found already existing Artifact type with the same external_id: ",
          record.DebugString()));
    }
  }

  return ExecuteQuerySelectLastInsertID(
      query_config_.insert_artifact_type(),
      {Bind(name), Bind(version), Bind(description), Bind(external_id)},
      type_id);
}

absl::Status QueryConfigExecutor::InsertExecutionType(
    const std::string& name, absl::optional<absl::string_view> version,
    absl::optional<absl::string_view> description,
    const ArtifactStructType* input_type, const ArtifactStructType* output_type,
    absl::optional<absl::string_view> external_id, int64* type_id) {
  // TODO(b/248836219): Cleanup the fat-client after fully migrated to V9+.
  if (query_schema_version().has_value() &&
      query_schema_version().value() < kSchemaVersionNine) {
    MetadataSourceQueryConfig::TemplateQuery insert_execution_type;
    MLMD_RETURN_IF_ERROR(GetTemplateQueryOrDie(
        entity_query::v7_and_v8::kInsertExecutionType, insert_execution_type));
    return ExecuteQuerySelectLastInsertID(
        insert_execution_type,
        {Bind(name), Bind(version), Bind(description), Bind(input_type),
         Bind(output_type)},
        type_id);
  }

  if (external_id.has_value()) {
    RecordSet record;
    MLMD_RETURN_IF_ERROR(
        ExecuteQuery(query_config_.select_types_by_external_ids(),
                     {Bind({external_id.value()}), Bind(0)}, &record));
    if (record.records_size() > 0) {
      return absl::AlreadyExistsError(absl::StrCat(
          "Conflict of external_id: ", external_id.value(),
          " Found already existing Execution type with the same external_id: ",
          record.DebugString()));
    }
  }
  return ExecuteQuerySelectLastInsertID(
      query_config_.insert_execution_type(),
      {Bind(name), Bind(version), Bind(description), Bind(input_type),
       Bind(output_type), Bind(external_id)},
      type_id);
}

absl::Status QueryConfigExecutor::InsertContextType(
    const std::string& name, absl::optional<absl::string_view> version,
    absl::optional<absl::string_view> description,
    absl::optional<absl::string_view> external_id, int64* type_id) {
  // TODO(b/248836219): Cleanup the fat-client after fully migrated to V9+.
  if (query_schema_version().has_value() &&
      query_schema_version().value() < kSchemaVersionNine) {
    MetadataSourceQueryConfig::TemplateQuery insert_context_type;
    MLMD_RETURN_IF_ERROR(GetTemplateQueryOrDie(
        entity_query::v7_and_v8::kInsertContextType, insert_context_type));
    return ExecuteQuerySelectLastInsertID(
        insert_context_type, {Bind(name), Bind(version), Bind(description)},
        type_id);
  }

  if (external_id.has_value()) {
    RecordSet record;
    MLMD_RETURN_IF_ERROR(
        ExecuteQuery(query_config_.select_types_by_external_ids(),
                     {Bind({external_id.value()}), Bind(2)}, &record));
    if (record.records_size() > 0) {
      return absl::AlreadyExistsError(absl::StrCat(
          "Conflict of external_id: ", external_id.value(),
          " Found already existing Context type with the same external_id: ",
          record.DebugString()));
    }
  }
  return ExecuteQuerySelectLastInsertID(
      query_config_.insert_context_type(),
      {Bind(name), Bind(version), Bind(description), Bind(external_id)},
      type_id);
}

absl::Status QueryConfigExecutor::SelectTypesByID(
    absl::Span<const int64> type_ids, TypeKind type_kind,
    RecordSet* record_set) {
  // TODO(b/248836219): Cleanup the fat-client after fully migrated to V9+.
  if (query_schema_version().has_value() &&
      query_schema_version().value() < kSchemaVersionNine) {
    MetadataSourceQueryConfig::TemplateQuery select_types_by_id;
    MLMD_RETURN_IF_ERROR(GetTemplateQueryOrDie(
        entity_query::v7_and_v8::kSelectTypesById, select_types_by_id));
    return ExecuteQuery(select_types_by_id, {Bind(type_ids), Bind(type_kind)},
                        record_set);
  }

  return ExecuteQuery(query_config_.select_types_by_id(),
                      {Bind(type_ids), Bind(type_kind)}, record_set);
}

absl::Status QueryConfigExecutor::SelectTypeByID(int64 type_id,
                                                 TypeKind type_kind,
                                                 RecordSet* record_set) {
  // TODO(b/248836219): Cleanup the fat-client after fully migrated to V9+.
  if (query_schema_version().has_value() &&
      query_schema_version().value() < kSchemaVersionNine) {
    MetadataSourceQueryConfig::TemplateQuery select_type_by_id;
    MLMD_RETURN_IF_ERROR(GetTemplateQueryOrDie(
        entity_query::v7_and_v8::kSelectTypeById, select_type_by_id));
    return ExecuteQuery(select_type_by_id, {Bind(type_id), Bind(type_kind)},
                        record_set);
  }
  return ExecuteQuery(query_config_.select_type_by_id(),
                      {Bind(type_id), Bind(type_kind)}, record_set);
}

absl::Status QueryConfigExecutor::SelectTypesByExternalIds(
    const absl::Span<absl::string_view> external_ids, TypeKind type_kind,
    RecordSet* record_set) {
  MLMD_RETURN_IF_ERROR(VerifyCurrentQueryVersionIsAtLeast(kSchemaVersionNine));
  return ExecuteQuery(query_config_.select_types_by_external_ids(),
                      {Bind(external_ids), Bind(type_kind)}, record_set);
}

absl::Status QueryConfigExecutor::SelectTypeByNameAndVersion(
    absl::string_view type_name, absl::optional<absl::string_view> type_version,
    TypeKind type_kind, RecordSet* record_set) {
  // TODO(b/248836219): Cleanup the fat-client after fully migrated to V9+.
  if (type_version && !type_version->empty()) {
    if (query_schema_version().has_value() &&
        query_schema_version().value() < kSchemaVersionNine) {
      MetadataSourceQueryConfig::TemplateQuery select_type_by_name_and_version;
      MLMD_RETURN_IF_ERROR(GetTemplateQueryOrDie(
          entity_query::v7_and_v8::kSelectTypeByNameAndVersion,
          select_type_by_name_and_version));
      return ExecuteQuery(
          select_type_by_name_and_version,
          {Bind(type_name), Bind(*type_version), Bind(type_kind)}, record_set);
    }
    return ExecuteQuery(query_config_.select_type_by_name_and_version(),
                        {Bind(type_name), Bind(*type_version), Bind(type_kind)},
                        record_set);
  } else {
    if (query_schema_version().has_value() &&
        query_schema_version().value() < kSchemaVersionNine) {
      MetadataSourceQueryConfig::TemplateQuery select_type_by_name;
      MLMD_RETURN_IF_ERROR(GetTemplateQueryOrDie(
          entity_query::v7_and_v8::kSelectTypeByName, select_type_by_name));
      return ExecuteQuery(select_type_by_name,
                          {Bind(type_name), Bind(type_kind)}, record_set);
    }
    return ExecuteQuery(query_config_.select_type_by_name(),
                        {Bind(type_name), Bind(type_kind)}, record_set);
  }
}

absl::Status QueryConfigExecutor::SelectTypesByNamesAndVersions(
    absl::Span<std::pair<std::string, std::string>> names_and_versions,
    TypeKind type_kind, RecordSet* record_set) {
  auto partition = std::partition(
      names_and_versions.begin(), names_and_versions.end(),
      [](std::pair<absl::string_view, absl::string_view> name_and_version) {
        return !name_and_version.second.empty();
      });
  // find types with both name and version
  if (names_and_versions.begin() != partition) {
    std::vector<std::pair<absl::string_view, absl::string_view>>
        names_with_versions = {names_and_versions.begin(), partition};
    MLMD_RETURN_IF_ERROR(ExecuteQuery(
        query_config_.select_types_by_names_and_versions(),
        {Bind(absl::MakeSpan(names_with_versions)), Bind(type_kind)},
        record_set));
  }
  // find types with names only
  if (partition != names_and_versions.end()) {
    std::vector<absl::string_view> names;
    std::transform(
        partition, names_and_versions.end(), std::back_inserter(names),
        [](const auto& pair) { return absl::string_view(pair.first); });
    RecordSet record_set_for_types_with_names_only;
    MLMD_RETURN_IF_ERROR(
        ExecuteQuery(query_config_.select_types_by_names(),
                     {Bind(absl::MakeSpan(names)), Bind(type_kind)},
                     &record_set_for_types_with_names_only));
    if (record_set->records_size() > 0) {
      record_set->mutable_records()->MergeFrom(
          record_set_for_types_with_names_only.records());
    } else {
      *record_set = std::move(record_set_for_types_with_names_only);
    }
  }

  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::SelectAllTypes(TypeKind type_kind,
                                                 RecordSet* record_set) {
  return ExecuteQuery(query_config_.select_all_types(), {Bind(type_kind)},
                      record_set);
}

template <typename Node>
absl::Status QueryConfigExecutor::ListNodeIDsUsingOptions(
    const ListOperationOptions& options,
    absl::optional<absl::Span<const int64>> candidate_ids,
    RecordSet* record_set) {
  // Skip query if candidate_ids are set with an empty collection.
  if (candidate_ids && candidate_ids->empty()) {
    return absl::OkStatus();
  }
  std::string sql_query;
  int64 query_version = query_schema_version().has_value()
                            ? query_schema_version().value()
                            : kSchemaVersionTen;
  absl::optional<absl::string_view> node_table_alias;
  if (std::is_same<Node, Artifact>::value) {
    sql_query = "SELECT `id` FROM `Artifact` WHERE";
  } else if (std::is_same<Node, Execution>::value) {
    sql_query = "SELECT `id` FROM `Execution` WHERE";
  } else if (std::is_same<Node, Context>::value) {
    sql_query = "SELECT `id` FROM `Context` WHERE";
  } else {
    return absl::InvalidArgumentError(
        "Invalid Node passed to ListNodeIDsUsingOptions");
  }
  // TODO(b/195700145) MLMD Filtering is not supported in Windows platform since
  // ZetaSQL currently does not compile on Windows.
#ifndef _WIN32
  if (options.has_filter_query() && !options.filter_query().empty()) {
    node_table_alias = ml_metadata::FilterQueryBuilder<Node>::kBaseTableAlias;
    ml_metadata::FilterQueryAstResolver<Node> ast_resolver(
        options.filter_query());
    const absl::Status ast_gen_status = ast_resolver.Resolve();
    if (!ast_gen_status.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid `filter_query`: ", ast_gen_status.message()));
    }
    // Generate SQL
    ml_metadata::FilterQueryBuilder<Node> query_builder;
    const absl::Status sql_gen_status =
        ast_resolver.GetAst()->Accept(&query_builder);
    if (!sql_gen_status.ok()) {
      return absl::InternalError(
          absl::StrCat("Failed to construct valid SQL from `filter_query`: ",
                       sql_gen_status.message()));
    }
    sql_query = absl::Substitute(
        "SELECT distinct $0.`id` FROM $1 WHERE $2 AND ", *node_table_alias,
        // TODO(b/257334039): remove query_version-conditional logic
        query_builder.GetFromClause(query_version),
        query_builder.GetWhereClause());
  }
#endif

  if (candidate_ids) {
    if (node_table_alias.has_value() && !node_table_alias->empty()) {
      absl::SubstituteAndAppend(&sql_query, " $0.`id`", *node_table_alias);
      absl::SubstituteAndAppend(&sql_query, " IN ($0) AND ",
                                Bind(*candidate_ids));
    } else {
      absl::SubstituteAndAppend(&sql_query, " `id` IN ($0) AND ",
                                Bind(*candidate_ids));
    }
  }
  MLMD_RETURN_IF_ERROR(
      AppendOrderingThresholdClause(options, node_table_alias, sql_query));
  MLMD_RETURN_IF_ERROR(
      AppendOrderByClause(options, node_table_alias, sql_query));
  MLMD_RETURN_IF_ERROR(AppendLimitClause(options, sql_query));
  return ExecuteQuery(sql_query, record_set);
}

absl::Status QueryConfigExecutor::ListArtifactIDsUsingOptions(
    const ListOperationOptions& options,
    absl::optional<absl::Span<const int64>> candidate_ids,
    RecordSet* record_set) {
  return ListNodeIDsUsingOptions<Artifact>(options, candidate_ids, record_set);
}

absl::Status QueryConfigExecutor::ListExecutionIDsUsingOptions(
    const ListOperationOptions& options,
    absl::optional<absl::Span<const int64>> candidate_ids,
    RecordSet* record_set) {
  return ListNodeIDsUsingOptions<Execution>(options, candidate_ids, record_set);
}

absl::Status QueryConfigExecutor::ListContextIDsUsingOptions(
    const ListOperationOptions& options,
    absl::optional<absl::Span<const int64>> candidate_ids,
    RecordSet* record_set) {
  return ListNodeIDsUsingOptions<Context>(options, candidate_ids, record_set);
}


absl::Status QueryConfigExecutor::DeleteExecutionsById(
    absl::Span<const int64> execution_ids) {
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.delete_executions_by_id(),
                                    {Bind(execution_ids)}));
  MLMD_RETURN_IF_ERROR(ExecuteQuery(
      query_config_.delete_executions_properties_by_executions_id(),
      {Bind(execution_ids)}));
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::DeleteArtifactsById(
    absl::Span<const int64> artifact_ids) {
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.delete_artifacts_by_id(),
                                    {Bind(artifact_ids)}));
  MLMD_RETURN_IF_ERROR(
      ExecuteQuery(query_config_.delete_artifacts_properties_by_artifacts_id(),
                   {Bind(artifact_ids)}));
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::DeleteContextsById(
    absl::Span<const int64> context_ids) {
  MLMD_RETURN_IF_ERROR(
      ExecuteQuery(query_config_.delete_contexts_by_id(), {Bind(context_ids)}));
  MLMD_RETURN_IF_ERROR(
      ExecuteQuery(query_config_.delete_contexts_properties_by_contexts_id(),
                   {Bind(context_ids)}));
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::DeleteEventsByArtifactsId(
    absl::Span<const int64> artifact_ids) {
  MLMD_RETURN_IF_ERROR(ExecuteQuery(
      query_config_.delete_events_by_artifacts_id(), {Bind(artifact_ids)}));
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.delete_event_paths()));
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::DeleteEventsByExecutionsId(
    absl::Span<const int64> execution_ids) {
  MLMD_RETURN_IF_ERROR(ExecuteQuery(
      query_config_.delete_events_by_executions_id(), {Bind(execution_ids)}));
  MLMD_RETURN_IF_ERROR(ExecuteQuery(query_config_.delete_event_paths()));
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::DeleteAttributionsByContextsId(
    absl::Span<const int64> context_ids) {
  MLMD_RETURN_IF_ERROR(ExecuteQuery(
      query_config_.delete_attributions_by_contexts_id(), {Bind(context_ids)}));
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::DeleteAttributionsByArtifactsId(
    absl::Span<const int64> artifact_ids) {
  MLMD_RETURN_IF_ERROR(
      ExecuteQuery(query_config_.delete_attributions_by_artifacts_id(),
                   {Bind(artifact_ids)}));
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::DeleteAssociationsByContextsId(
    absl::Span<const int64> context_ids) {
  MLMD_RETURN_IF_ERROR(ExecuteQuery(
      query_config_.delete_associations_by_contexts_id(), {Bind(context_ids)}));
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::DeleteAssociationsByExecutionsId(
    absl::Span<const int64> execution_ids) {
  MLMD_RETURN_IF_ERROR(
      ExecuteQuery(query_config_.delete_associations_by_executions_id(),
                   {Bind(execution_ids)}));
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::DeleteParentContextsByParentIds(
    absl::Span<const int64> parent_context_ids) {
  MLMD_RETURN_IF_ERROR(
      ExecuteQuery(query_config_.delete_parent_contexts_by_parent_ids(),
                   {Bind(parent_context_ids)}));
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::DeleteParentContextsByChildIds(
    absl::Span<const int64> child_context_ids) {
  MLMD_RETURN_IF_ERROR(
      ExecuteQuery(query_config_.delete_parent_contexts_by_child_ids(),
                   {Bind(child_context_ids)}));
  return absl::OkStatus();
}

absl::Status QueryConfigExecutor::DeleteParentContextsByParentIdAndChildIds(
    int64 parent_context_id, absl::Span<const int64> child_context_ids) {
  MLMD_RETURN_IF_ERROR(ExecuteQuery(
      query_config_.delete_parent_contexts_by_parent_id_and_child_ids(),
      {Bind(parent_context_id), Bind(child_context_ids)}));
  return absl::OkStatus();
}

}  // namespace ml_metadata
