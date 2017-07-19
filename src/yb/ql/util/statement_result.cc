//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//
// Different results of processing a statement.
//--------------------------------------------------------------------------------------------------

#include "yb/ql/util/statement_result.h"

#include "yb/client/client.h"
#include "yb/client/schema-internal.h"
#include "yb/common/wire_protocol.h"
#include "yb/util/pb_util.h"
#include "yb/ql/ptree/pt_select.h"

namespace yb {
namespace ql {

using std::string;
using std::vector;
using std::unique_ptr;
using std::shared_ptr;
using std::make_shared;
using strings::Substitute;

using client::YBOperation;
using client::YBqlOp;
using client::YBqlReadOp;
using client::YBqlWriteOp;

//------------------------------------------------------------------------------------------------
namespace {

// Get bind column schemas for DML.
vector<ColumnSchema> GetBindVariableSchemasFromDmlStmt(const PTDmlStmt& stmt) {
  vector<ColumnSchema> bind_variable_schemas;
  bind_variable_schemas.reserve(stmt.bind_variables().size());
  for (const PTBindVar *var : stmt.bind_variables()) {
    bind_variable_schemas.emplace_back(string(var->name()->c_str()), var->ql_type());
  }
  return bind_variable_schemas;
}

shared_ptr<vector<ColumnSchema>> GetColumnSchemasFromOp(const YBqlOp& op, const PTDmlStmt *tnode) {
  switch (op.type()) {
    case YBOperation::Type::QL_READ: {
      // For actual execution "tnode" is always not null.
      if (tnode != nullptr) {
        return tnode->selected_schemas();
      }

      // Tests don't have access to the QL internal statement object, so they have to use rsrow
      // descriptor from the read request.
      const QLRSRowDescPB& rsrow_desc = static_cast<const YBqlReadOp&>(op).request().rsrow_desc();
      shared_ptr<vector<ColumnSchema>> column_schemas = make_shared<vector<ColumnSchema>>();
      for (const auto& rscol_desc : rsrow_desc.rscol_descs()) {
        column_schemas->emplace_back(rscol_desc.name(),
                                     QLType::FromQLTypePB(rscol_desc.ql_type()));
      }
      return column_schemas;
    }

    case YBOperation::Type::QL_WRITE: {
      shared_ptr<vector<ColumnSchema>> column_schemas = make_shared<vector<ColumnSchema>>();
      const auto& write_op = static_cast<const YBqlWriteOp&>(op);
      column_schemas->reserve(write_op.response().column_schemas_size());
      for (const auto column_schema : write_op.response().column_schemas()) {
        column_schemas->emplace_back(ColumnSchemaFromPB(column_schema));
      }
      return column_schemas;
    }

    case YBOperation::Type::INSERT: FALLTHROUGH_INTENDED;
    case YBOperation::Type::UPDATE: FALLTHROUGH_INTENDED;
    case YBOperation::Type::DELETE: FALLTHROUGH_INTENDED;
    case YBOperation::Type::REDIS_READ: FALLTHROUGH_INTENDED;
    case YBOperation::Type::REDIS_WRITE:
      break;
    // default: fallthrough
  }

  LOG(FATAL) << "Internal error: invalid or unknown QL operation: " << op.type();
  return nullptr;
}

QLClient GetClientFromOp(const YBqlOp& op) {
  switch (op.type()) {
    case YBOperation::Type::QL_READ:
      return static_cast<const YBqlReadOp&>(op).request().client();
    case YBOperation::Type::QL_WRITE:
      return static_cast<const YBqlWriteOp&>(op).request().client();
    case YBOperation::Type::INSERT: FALLTHROUGH_INTENDED;
    case YBOperation::Type::UPDATE: FALLTHROUGH_INTENDED;
    case YBOperation::Type::DELETE: FALLTHROUGH_INTENDED;
    case YBOperation::Type::REDIS_READ: FALLTHROUGH_INTENDED;
    case YBOperation::Type::REDIS_WRITE:
      break;
    // default: fallthrough
  }
  LOG(FATAL) << "Internal error: invalid or unknown QL operation: " << op.type();

  // Inactive code: It's only meant to avoid compilation warning.
  return QLClient();
}

} // namespace

//------------------------------------------------------------------------------------------------
PreparedResult::PreparedResult(const PTDmlStmt& stmt)
    : table_name_(stmt.table()->name()),
      hash_col_indices_(stmt.hash_col_indices()),
      bind_variable_schemas_(GetBindVariableSchemasFromDmlStmt(stmt)),
      column_schemas_(stmt.selected_schemas()) {
  if (column_schemas_ == nullptr) {
    column_schemas_ = make_shared<vector<ColumnSchema>>();
  }
}

PreparedResult::~PreparedResult() {
}

//------------------------------------------------------------------------------------------------
RowsResult::RowsResult(YBqlOp *op, const PTDmlStmt *tnode)
    : table_name_(op->table()->name()),
      column_schemas_(GetColumnSchemasFromOp(*op, tnode)),
      client_(GetClientFromOp(*op)),
      rows_data_(op->rows_data()) {

  if (column_schemas_ == nullptr) {
    column_schemas_ = make_shared<vector<ColumnSchema>>();
  }

  // If there is a paging state in the response, fill in the table ID also and serialize the
  // paging state as bytes.
  if (op->response().has_paging_state()) {
    QLPagingStatePB *paging_state = op->mutable_response()->mutable_paging_state();
    paging_state->set_table_id(op->table()->id());
    faststring serialized_paging_state;
    CHECK(pb_util::SerializeToString(*paging_state, &serialized_paging_state));
    paging_state_ = serialized_paging_state.ToString();
  }
}

RowsResult::RowsResult(const client::YBTableName& table_name,
                       const shared_ptr<vector<ColumnSchema>>& column_schemas,
                       const std::string& rows_data)
    : table_name_(table_name),
      column_schemas_(column_schemas),
      client_(QLClient::YQL_CLIENT_CQL),
      rows_data_(rows_data) {
}

RowsResult::~RowsResult() {
}

Status RowsResult::Append(const RowsResult& other) {
  if (rows_data_.empty()) {
    rows_data_ = other.rows_data_;
  } else {
    RETURN_NOT_OK(QLRowBlock::AppendRowsData(other.client_, other.rows_data_, &rows_data_));
  }
  paging_state_ = other.paging_state_;
  return Status::OK();
}

std::unique_ptr<QLRowBlock> RowsResult::GetRowBlock() const {
  Schema schema(*column_schemas_, 0);
  unique_ptr<QLRowBlock> rowblock(new QLRowBlock(schema));
  Slice data(rows_data_);
  if (!data.empty()) {
    // TODO: a better way to handle errors here?
    CHECK_OK(rowblock->Deserialize(client_, &data));
  }
  return rowblock;
}

//------------------------------------------------------------------------------------------------
SchemaChangeResult::SchemaChangeResult(
    const string& change_type, const string& object_type,
    const string& keyspace_name, const string& object_name)
    : change_type_(change_type), object_type_(object_type),
      keyspace_name_(keyspace_name), object_name_(object_name) {
}

SchemaChangeResult::~SchemaChangeResult() {
}


} // namespace ql
} // namespace yb
