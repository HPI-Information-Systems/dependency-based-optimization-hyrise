#include "meta_dependencies_table.hpp"

#include <memory>
#include <sstream>
#include <unordered_map>

#include "magic_enum.hpp"

#include "all_type_variant.hpp"
#include "hyrise.hpp"
#include "storage/constraints/foreign_key_constraint.hpp"
#include "storage/constraints/table_key_constraint.hpp"
#include "storage/constraints/table_order_constraint.hpp"
#include "storage/table.hpp"
#include "types.hpp"

namespace {

using namespace hyrise;  // NOLINT(build/namespaces)

template <typename Container>
std::string column_names(const std::shared_ptr<Table>& table, const Container& columns) {
  Assert(!columns.empty(), "Did not expect useless constraint.");
  auto stream = std::stringstream{};
  const auto last_column = *std::prev(columns.cend());
  for (auto column : columns) {
    stream << table->column_name(column);
    if (column != last_column) {
      stream << ", ";
    }
  }

  return stream.str();
}

pmr_string print_ucc(const std::shared_ptr<Table>& table, const TableKeyConstraint& ucc) {
  auto stream = std::stringstream{};
  stream << "{ " << column_names(table, ucc.columns()) << " }";
  return pmr_string{stream.str()};
}

pmr_string print_od(const std::shared_ptr<Table>& table, const TableOrderConstraint& od) {
  auto stream = std::stringstream{};
  stream << "[ " << column_names(table, od.ordering_columns()) << " ] |-> [ "
         << column_names(table, od.ordered_columns()) << " ]";
  return pmr_string{stream.str()};
}

pmr_string print_ind(const ForeignKeyConstraint& ind) {
  auto stream = std::stringstream{};
  stream << "[ " << column_names(ind.foreign_key_table(), ind.foreign_key_columns()) << " ] IN [ "
         << column_names(ind.primary_key_table(), ind.primary_key_columns()) << " ]";
  return pmr_string{stream.str()};
}

}  // namespace

namespace hyrise {

MetaDependenciesTable::MetaDependenciesTable()
    : AbstractMetaTable(TableColumnDefinitions{{"table_name", DataType::String, false},
                                               {"referenced_table_name", DataType::String, true},
                                               {"type", DataType::String, false},
                                               {"description", DataType::String, false}}) {}

const std::string& MetaDependenciesTable::name() const {
  static const auto name = std::string{"data_dependencies"};
  return name;
}

std::shared_ptr<Table> MetaDependenciesTable::_on_generate() const {
  auto output_table = std::make_shared<Table>(_column_definitions, TableType::Data, std::nullopt, UseMvcc::Yes);

  // Create mapping table -> name to access references table names.
  const auto tables = Hyrise::get().storage_manager.tables();
  auto table_names = std::unordered_map<std::shared_ptr<Table>, pmr_string>(tables.size());
  for (const auto& [table_name, table] : tables) {
    table_names.emplace(table, table_name);
  }

  for (const auto& [table_name, table] : tables) {
    for (const auto& ucc : table->soft_key_constraints()) {
      output_table->append({pmr_string{table_name}, NULL_VALUE, pmr_string{"UCC"}, print_ucc(table, ucc)});
    }

    for (const auto& od : table->soft_order_constraints()) {
      output_table->append({pmr_string{table_name}, NULL_VALUE, pmr_string{"OD"}, print_od(table, od)});
    }

    for (const auto& ind : table->soft_foreign_key_constraints()) {
      output_table->append(
          {pmr_string{table_name}, table_names[ind.primary_key_table()], pmr_string{"IND"}, print_ind(ind)});
    }
  }

  return output_table;
}

}  // namespace hyrise
