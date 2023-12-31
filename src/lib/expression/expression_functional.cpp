#include "expression_functional.hpp"

namespace hyrise::expression_functional {

std::shared_ptr<AbstractExpression> to_expression(const std::shared_ptr<AbstractExpression>& expression) {
  return expression;
}

std::shared_ptr<ValueExpression> to_expression(const AllTypeVariant& value) {
  return std::make_shared<ValueExpression>(value);
}

std::shared_ptr<ValueExpression> value_(const AllTypeVariant& value) {  // NOLINT - clang-tidy doesn't like the suffix
  return std::make_shared<ValueExpression>(value);
}

// otherwise the NOLINT markers get misplaced
// clang-format off
std::shared_ptr<ValueExpression> null_() { // NOLINT - clang-tidy doesn't like the suffix
  return std::make_shared<ValueExpression>(NullValue{});
}

std::shared_ptr<PlaceholderExpression> placeholder_(const ParameterID parameter_id) {  // NOLINT - clang-tidy doesn't like the suffix
  return std::make_shared<PlaceholderExpression>(parameter_id);
}

std::shared_ptr<LQPColumnExpression> lqp_column_(const std::shared_ptr<const AbstractLQPNode>& original_node, const ColumnID original_column_id) {  // NOLINT - clang-tidy doesn't like the suffix
  return std::make_shared<LQPColumnExpression>(original_node, original_column_id);
}

std::shared_ptr<PQPColumnExpression> pqp_column_(const ColumnID column_id, const DataType data_type, const bool nullable,  // NOLINT - clang-tidy doesn't like the suffix
                                             const std::string& column_name) {
  return std::make_shared<PQPColumnExpression>(column_id, data_type, nullable, column_name);
}

std::shared_ptr<AggregateExpression> count_star_(const std::shared_ptr<AbstractLQPNode>& lqp_node) {  // NOLINT - clang-tidy doesn't like the suffix
  const auto column_expression = std::make_shared<LQPColumnExpression>(lqp_node, INVALID_COLUMN_ID);
  return std::make_shared<AggregateExpression>(AggregateFunction::Count, column_expression);
}

std::shared_ptr<ExistsExpression> exists_(const std::shared_ptr<AbstractExpression>& subquery_expression) {  // NOLINT - clang-tidy doesn't like the suffix
  return std::make_shared<ExistsExpression>(subquery_expression, ExistsExpressionType::Exists);
}

std::shared_ptr<ExistsExpression> not_exists_(const std::shared_ptr<AbstractExpression>& subquery_expression) {  // NOLINT - clang-tidy doesn't like the suffix
  return std::make_shared<ExistsExpression>(subquery_expression, ExistsExpressionType::NotExists);
}

std::shared_ptr<IntervalExpression> interval_(const int64_t duration, const DatetimeComponent unit) {  // NOLINT - clang-tidy doesn't like the suffix
  return std::make_shared<IntervalExpression>(duration, unit);
}

// clang-format on

}  // namespace hyrise::expression_functional
