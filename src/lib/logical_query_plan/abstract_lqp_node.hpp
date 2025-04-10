#pragma once

#include <array>
#include <unordered_map>

#include "enable_make_for_lqp_node.hpp"
#include "logical_query_plan/data_dependencies/functional_dependency.hpp"
#include "logical_query_plan/data_dependencies/inclusion_dependency.hpp"
#include "logical_query_plan/data_dependencies/order_dependency.hpp"
#include "logical_query_plan/data_dependencies/unique_column_combination.hpp"

namespace hyrise {

enum class LQPNodeType {
  Aggregate,
  Alias,
  ChangeMetaTable,
  CreateTable,
  CreatePreparedPlan,
  CreateView,
  Delete,
  DropView,
  DropTable,
  DummyTable,
  Except,
  Export,
  Import,
  Insert,
  Intersect,
  Join,
  Limit,
  Predicate,
  Projection,
  Root,
  Sort,
  StaticTable,
  StoredTable,
  Update,
  Union,
  Validate,
  Mock
};

enum class LQPInputSide { Left, Right };

// Describes the output of a Node and which of the output's inputs this Node is
struct LQPOutputRelation {
  std::shared_ptr<AbstractLQPNode> output;
  LQPInputSide input_side{LQPInputSide::Left};
};

using LQPNodeMapping = std::unordered_map<std::shared_ptr<const AbstractLQPNode>, std::shared_ptr<AbstractLQPNode>>;

class LQPColumnExpression;

class AbstractLQPNode : public std::enable_shared_from_this<AbstractLQPNode> {
 public:
  AbstractLQPNode(const LQPNodeType node_type,
                  const std::vector<std::shared_ptr<AbstractExpression>>& init_node_expressions = {});
  virtual ~AbstractLQPNode();

  /**
   * @return a string describing this node, but nothing about its inputs.
   */
  enum class DescriptionMode { Short, Detailed };
  virtual std::string description(const DescriptionMode mode = DescriptionMode::Short) const = 0;

  /**
   * @defgroup Access the outputs/inputs
   *
   * The outputs are implicitly set and removed in set_left_input()/set_right_input()/set_input().
   *
   * set_input() is a shorthand for set_left_input() or set_right_input(), useful if the side is a runtime value.
   * @{
   */
  std::shared_ptr<AbstractLQPNode> left_input() const;
  std::shared_ptr<AbstractLQPNode> right_input() const;
  std::shared_ptr<AbstractLQPNode> input(LQPInputSide side) const;
  void set_left_input(const std::shared_ptr<AbstractLQPNode>& left);
  void set_right_input(const std::shared_ptr<AbstractLQPNode>& right);
  void set_input(LQPInputSide side, const std::shared_ptr<AbstractLQPNode>& input);

  size_t input_count() const;

  /**
   * @pre this has @param output as an output
   * @return whether this is the left or right input in the specified output.
   */
  LQPInputSide get_input_side(const std::shared_ptr<AbstractLQPNode>& output) const;

  /**
   * @return {get_output_side(outputs()[0], ..., get_output_side(outputs()[n-1])}
   */
  std::vector<LQPInputSide> get_input_sides() const;

  /**
   * Locks all outputs (as they are stored in weak_ptrs) and returns them as shared_ptrs
   */
  std::vector<std::shared_ptr<AbstractLQPNode>> outputs() const;

  void remove_output(const std::shared_ptr<AbstractLQPNode>& output);
  void clear_outputs();

  /**
   * @return {{outputs()[0], get_input_sides()[0]}, ..., {outputs()[n-1], get_input_sides()[n-1]}}
   */
  std::vector<LQPOutputRelation> output_relations() const;

  /**
   * Same as outputs().size(), but avoids locking all output pointers
   */
  size_t output_count() const;
  /** @} */

  /**
   * @param input_node_mapping     If the LQP contains external expressions, a mapping for the nodes used by them needs
   *                               to be provided.
   * @return                       A deep copy of the LQP this Node is the root of
   */
  std::shared_ptr<AbstractLQPNode> deep_copy(LQPNodeMapping input_node_mapping = {}) const;

  /**
   * Compare this node with another, without comparing inputs.
   * @param node_mapping    Mapping from nodes in this node's input plans to corresponding nodes in the input plans of
   *                        rhs
   */
  bool shallow_equals(const AbstractLQPNode& rhs, const LQPNodeMapping& node_mapping) const;

  /**
   * @return The Expressions defining each "column" that this node outputs. Note: When talking about LQPs, we use the
   *         term expression, rather than column. A ProjectionNode might output `a + 5`, where a is an
   *         LQPColumnExpression and `a + 5` is an ArithmeticExpression. Avoid "column expression" if you do not mean
   *         a column that comes from an actual table.
   */
  virtual std::vector<std::shared_ptr<AbstractExpression>> output_expressions() const;

  /**
   * @return The ColumnID of the @param expression, or std::nullopt if it cannot be found. Note that because COUNT(*)
   *         has a special treatment (it is represented as an LQPColumnExpression with an INVALID_COLUMN_ID), it might
   *         be evaluable even if find_column_id returns nullopt.
   */
  std::optional<ColumnID> find_column_id(const AbstractExpression& expression) const;

  /**
   * @return The ColumnID of the @param expression. Assert()s that it can be found
   */
  ColumnID get_column_id(const AbstractExpression& expression) const;

  /**
   * @return True, if the given set of expressions is a subset of the node's output expressions. False otherwise.
   */
  bool has_output_expressions(const ExpressionUnorderedSet& expressions) const;

  enum class ExpressionIteration { Continue, Break };

  /**
   * Calls the passed @param visitor on each of the output expressions.
   * The visitor returns `ExpressionIteration`, indicating whether the remaining expressions should be visited as well.
   * Prefer this method over multiple calls of `find_column_id()` or `get_column_id()`, as it computes the output
   * expressions only once.
   *
   * @tparam Visitor      Functor called with ColumnID and the expression as a param.
   *                      Returns `ExpressionIteration`
   */
  template <typename Visitor>
  void iterate_output_expressions(Visitor visitor) const {
    const auto& output_expressions = this->output_expressions();
    const auto output_expression_count = output_expressions.size();
    for (auto column_id = ColumnID{0}; column_id < output_expression_count; ++column_id) {
      if (visitor(column_id, output_expressions[column_id]) == ExpressionIteration::Break) {
        break;
      }
    }
  }

  /**
   * @return whether the output column at @param column_id is nullable
   */
  virtual bool is_column_nullable(const ColumnID column_id) const;

  /**
   * @return Unique column combinations (UCCs) valid for the current LQP. See unique_column_combination.hpp for more
   *         documentation.
   */
  virtual UniqueColumnCombinations unique_column_combinations() const = 0;

  /**
   * @return True if there is a unique column combination (UCC) matching the given subset of output expressions (i.e.,
   *         the rows are guaranteed to be unique). This is preferred over calling
   *         contains_matching_unique_column_combination(unique_column_combinations(), ...) as it performs additional
   *         sanity checks.
   */
  bool has_matching_ucc(const ExpressionUnorderedSet& expressions) const;

  /**
   * @return The functional dependencies valid for this node. See functional_dependency.hpp for documentation.
   *         They are collected from two different sources:
   *          (1) FDs derived from the node's unique column combinations (trivial FDs).
   *          (2) FDs provided by the child nodes (non-trivial FDs).
   */
  FunctionalDependencies functional_dependencies() const;

  /**
   * This is a helper method that returns non-trivial FDs valid for the current node. We consider FDs as non-trivial if
   *  we cannot derive them from the current node's unique column combinations.
   *
   * @return The default implementation returns non-trivial FDs from the left input node, if available. Otherwise an
   *         empty vector.
   *
   * Nodes should override this function
   *  - to add additional non-trivial FDs. For example, {a} -> {a + 1} (which is not yet implemented).
   *  - to discard non-trivial FDs from the input nodes, if necessary.
   *  - to specify forwarding of non-trivial FDs in case of two input nodes.
   */
  virtual FunctionalDependencies non_trivial_functional_dependencies() const;

  virtual OrderDependencies order_dependencies() const = 0;

  /**
   * @return True if there is an order dependency (OD) matching the given lists of output expressions (i.e., sorting
   *         the table by @param `ordering_expressions` also sorts @param `ordered_expressions`). This is preferred
   *         over calling contains_matching_order_dependency(order_dependencies(), ...) as it performs additional
   *         sanity checks.
   */
  bool has_matching_od(const std::vector<std::shared_ptr<AbstractExpression>>& ordering_expressions,
                       const std::vector<std::shared_ptr<AbstractExpression>>& ordered_expressions) const;

  virtual InclusionDependencies inclusion_dependencies() const = 0;

  /**
   * @return True if there is an inclusion dependency (IND) matching the given subset of output expressions whose
   *         included columns are also part of @param included_node's output expressions.
   */
  bool has_matching_ind(const ExpressionUnorderedSet& foreign_key_expressions,
                        const ExpressionUnorderedSet& key_expressions, const AbstractLQPNode& included_node) const;

  /**
   * Perform a deep equality check
   */
  bool operator==(const AbstractLQPNode& rhs) const;
  bool operator!=(const AbstractLQPNode& rhs) const;

  /**
   * @return a hash for the (sub)plan whose root this node is
   */
  size_t hash() const;

  const LQPNodeType type;

  /**
   * Expressions used by this node; semantics depend on the actual node type.
   * E.g., for the PredicateNode, this will be a single predicate expression; for a ProjectionNode it holds one
   * expression for each column.
   *
   * WARNING: When changing the length of this vector, **absolutely make sure** any data associated with the expressions
   *          (e.g. column names in the AliasNode, SortModes in the SortNode) gets adjusted accordingly.
   */
  std::vector<std::shared_ptr<AbstractExpression>> node_expressions;

  /**
   * Holds a (short) comment that is printed during plan visualization. For example, this could be a comment added by
   * the optimizer explaining that a node was added as a semi-join reduction node (see SubqueryToJoinRule). It is not
   * automatically added to the description.
   */
  std::string comment{};

 protected:
  /**
   * Override to hash data fields in derived types. No override needed if derived expression has no
   * data members. We do not need to take care of the input nodes here since they are already handled
   * by the calling methods.
   */
  virtual size_t _on_shallow_hash() const;
  virtual std::shared_ptr<AbstractLQPNode> _on_shallow_copy(LQPNodeMapping& node_mapping) const = 0;
  virtual bool _on_shallow_equals(const AbstractLQPNode& rhs, const LQPNodeMapping& node_mapping) const = 0;

  /**
   * This is a helper method for node types that do not have an effect on the UCCs from input nodes.
   * @return All unique column combinations from the left input node.
   */
  UniqueColumnCombinations _forward_left_unique_column_combinations() const;

  OrderDependencies _forward_left_order_dependencies() const;

  InclusionDependencies _forward_left_inclusion_dependencies() const;

  /*
   * Converts an AbstractLQPNode::DescriptionMode to an AbstractExpression::DescriptionMode
   */
  static AbstractExpression::DescriptionMode _expression_description_mode(const DescriptionMode mode);

 private:
  std::shared_ptr<AbstractLQPNode> _deep_copy_impl(LQPNodeMapping& node_mapping) const;
  std::shared_ptr<AbstractLQPNode> _shallow_copy(LQPNodeMapping& node_mapping) const;

  /**
   * @{
   * For internal usage in set_left_input(), set_right_input(), set_input(), remove_output()
   * Add or remove a output without manipulating this output's input ptr.
   */
  void _add_output_pointer(const std::shared_ptr<AbstractLQPNode>& output);
  void _remove_output_pointer(const AbstractLQPNode& output);
  /** @} */

  std::vector<std::weak_ptr<AbstractLQPNode>> _outputs;
  std::array<std::shared_ptr<AbstractLQPNode>, 2> _inputs;
};

std::ostream& operator<<(std::ostream& stream, const AbstractLQPNode& node);

// Wrapper around node->hash(), to enable hash based containers containing std::shared_ptr<AbstractLQPNode>
struct LQPNodeSharedPtrHash final {
  size_t operator()(const std::shared_ptr<AbstractLQPNode>& node) const {
    return node->hash();
  }
};

// Wrapper around AbstractLQPNode::operator==(), to enable hash based containers containing
// std::shared_ptr<AbstractLQPNode>
struct LQPNodeSharedPtrEqual final {
  size_t operator()(const std::shared_ptr<AbstractLQPNode>& lhs, const std::shared_ptr<AbstractLQPNode>& rhs) const {
    return lhs == rhs || *lhs == *rhs;
  }
};

// Note that operator== ignores the equality function:
// https://stackoverflow.com/questions/36167764/can-not-compare-stdunorded-set-with-custom-keyequal
template <typename Value>
using LQPNodeUnorderedMap =
    std::unordered_map<std::shared_ptr<AbstractLQPNode>, Value, LQPNodeSharedPtrHash, LQPNodeSharedPtrEqual>;

}  // namespace hyrise
