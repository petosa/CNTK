
#include <fstream>
#include <iostream>
#include <numeric>
#include <stack>

#include "proto/onnx/core/common/CommonSTD.h"
#include "graph.h"
// #include "gsl/pointers"
#include "op.h"
#include "utils.h"
#include "proto/onnx/core/common/logging/logging.h"
#include "proto/onnx/onnx/checker.h"
#include "proto/onnx/core/graph/schema_registry.h"

using namespace onnx::Utils;
using namespace onnx::checker;

namespace ONNXIR
{

#define NO_CHANGE_ON_SYNC_FLAG(...)                      \
    do                                                   \
    {                                                    \
        const bool sync_needed = GraphProtoSyncNeeded(); \
        {                                                \
            __VA_ARGS__;                                 \
        }                                                \
        GraphProtoSyncNeeded(sync_needed);               \
    } while (0)

NodeArg::NodeArg(const std::string& name,
                 const TypeProto* p_node_arg_type)
{
    node_arg_info_.set_name(name);
    // If the name is empty, it means the arg does not exist.
    exists_ = !(name.empty());
    if (nullptr != p_node_arg_type)
    {
        (*node_arg_info_.mutable_type()) = *p_node_arg_type;
        type_ = DataTypeUtils::ToType(node_arg_info_.type());
    }
    else
    {
        type_ = nullptr;
    }
}

const std::string& NodeArg::Name() const noexcept
{
    return node_arg_info_.name();
}

const DataType NodeArg::Type() const noexcept
{
    return type_;
}

const TypeProto* NodeArg::TypeAsProto() const noexcept
{
    if (node_arg_info_.has_type())
        return &node_arg_info_.type();
    else
        return nullptr;
}

const TensorShapeProto* NodeArg::Shape() const
{
    if (!node_arg_info_.has_type())
    {
        return nullptr;
    }

    const auto typeCase = node_arg_info_.type().value_case();
    switch (typeCase)
    {
    case TypeProto::kTensorType:
    {
        if (node_arg_info_.type().tensor_type().has_shape())
        {
            return &(node_arg_info_.type().tensor_type().shape());
        }
        else
        {
            return nullptr;
        }
    }
    case TypeProto::kSequenceType:
    case TypeProto::kMapType:
    case TypeProto::VALUE_NOT_SET:
    default:
        return nullptr;
    }
}

void NodeArg::SetShape(const TensorShapeProto& shape)
{
    if (!node_arg_info_.has_type())
    {
        return;
    }

    const auto type_case = node_arg_info_.type().value_case();
    switch (type_case)
    {
    case TypeProto::kTensorType:
        *(node_arg_info_.mutable_type()->mutable_tensor_type()->mutable_shape()) = shape;
        break;
    case TypeProto::kSequenceType:
    case TypeProto::kMapType:
    case TypeProto::VALUE_NOT_SET:
    default:
        return;
    }
}

void NodeArg::SetType(DataType p_type)
{
    if (nullptr == p_type)
    {
        return;
    }

    type_ = p_type;
    *(node_arg_info_.mutable_type()) = DataTypeUtils::ToTypeProto(p_type);
}

void NodeArg::SetType(const TypeProto& type_proto)
{
    type_ = DataTypeUtils::ToType(type_proto);
    *(node_arg_info_.mutable_type()) = type_proto;
}

bool NodeArg::Exists() const noexcept
{
    return exists_;
}

Node::EdgeEnd::EdgeEnd(const Node& node, const NodeArg& node_arg) noexcept
    : node_(&node), node_arg_(&node_arg)
{
}

const Node& Node::EdgeEnd::GetNode() const noexcept
{
    return *node_;
}

const NodeArg& Node::EdgeEnd::GetNodeArg() const noexcept
{
    return *node_arg_;
}

NodeIndex Node::Index() const noexcept
{
    return index_;
}

const std::string& Node::Name() const noexcept
{
    return name_;
}

const std::string& Node::OpType() const noexcept
{
    return op_type_;
}

const std::string& Node::Description() const noexcept
{
    return description_;
}

const std::string& Node::Domain() const noexcept
{
    return domain_;
}

const OpSchema* Node::Op() const noexcept
{
    return op_;
}

const std::string& Node::GetExecutionProviderType() const noexcept
{
    return execution_provider_type_;
}

void Node::SetExecutionProviderType(ONNXIR::ProviderType execution_provider_type)
{
    execution_provider_type_ = execution_provider_type;
}

void Node::ToProto(NodeProto& proto) const
{
    // Set name.
    proto.set_name(name_);
    // Set op type.
    proto.set_op_type(op_type_);
    // Set op domain;
    proto.set_domain(domain_);
    // Set doc string.
    proto.set_doc_string(description_);

    // Set attributes.
    proto.clear_attribute();
    for (auto attribute : attributes_)
    {
        auto attr = proto.add_attribute();
        *attr = attribute.second;
    }

    // Set inputs' definitions.
    proto.clear_input();
    for (auto& input_def : definitions_.input_defs)
    {
        proto.add_input(input_def->Name());
    }

    // Set outputs' definitions.
    proto.clear_output();
    for (auto& output_def : definitions_.output_defs)
    {
        proto.add_output(output_def->Name());
    }
}

void Node::Init(const std::string& name,
                const std::string& op_type,
                const std::string& description,
                const std::vector<NodeArg*>& input_args,
                const std::vector<NodeArg*>& output_args,
                const NodeAttributes* attributes,
                const std::string& domain)
{
    name_ = name;
    op_type_ = op_type;
    description_ = description;
    definitions_.input_defs = input_args;
    definitions_.output_defs = output_args;
    domain_ = domain;

    // Set each arg count as 1 by default.
    // It could be adjusted when resolving the node with its operator
    // information.
    definitions_.input_arg_count.assign(input_args.size(), 1);

    if (attributes)
    {
        attributes_ = *attributes;
    }
}

Node::Definitions& Node::MutableDefinitions() noexcept
{
    // someone fetching these is going to change something
    graph_->SetGraphResolveNeeded();
    graph_->SetGraphProtoSyncNeeded();
    return definitions_;
}

Node::Relationships& Node::MutableRelationships() noexcept
{
    // someone fetching these is going to change something
    graph_->SetGraphResolveNeeded();
    graph_->SetGraphProtoSyncNeeded();
    return relationships_;
}

void Node::AddAttribute(const std::string& attr_name, const AttributeProto& value)
{
    graph_->SetGraphResolveNeeded();
    graph_->SetGraphProtoSyncNeeded();
    attributes_[attr_name] = value;
}

#define ADD_BASIC_ATTR_IMPL(type, enumType, field)                           \
    void Node::AddAttribute(const std::string& attr_name, const type& value) \
    {                                                                        \
        graph_->SetGraphResolveNeeded();                                     \
        graph_->SetGraphProtoSyncNeeded();                                   \
        AttributeProto a;                                                    \
        a.set_name(attr_name);                                               \
        a.set_type(enumType);                                                \
        a.set_##field(value);                                                \
        attributes_[attr_name] = a;                                          \
    };

#define ADD_ATTR_IMPL(type, enumType, field)                                 \
    void Node::AddAttribute(const std::string& attr_name, const type& value) \
    {                                                                        \
        graph_->SetGraphResolveNeeded();                                     \
        graph_->SetGraphProtoSyncNeeded();                                   \
        AttributeProto a;                                                    \
        a.set_name(attr_name);                                               \
        a.set_type(enumType);                                                \
        *(a.mutable_##field()) = value;                                      \
        attributes_[attr_name] = a;                                          \
    };

#define ADD_LIST_ATTR_IMPL(type, enumType, field)            \
    void Node::AddAttribute(const std::string& attr_name,    \
                            const std::vector<type>& values) \
    {                                                        \
        graph_->SetGraphResolveNeeded();                     \
        graph_->SetGraphProtoSyncNeeded();                   \
        AttributeProto a;                                    \
        a.set_name(attr_name);                               \
        a.set_type(enumType);                                \
        for (const auto& val : values)                       \
        {                                                    \
            *(a.mutable_##field()->Add()) = val;             \
        }                                                    \
        attributes_[attr_name] = a;                          \
    };

ADD_BASIC_ATTR_IMPL(float, AttributeProto_AttributeType::AttributeProto_AttributeType_FLOAT, f)
ADD_BASIC_ATTR_IMPL(int64_t, AttributeProto_AttributeType::AttributeProto_AttributeType_INT, i)
ADD_BASIC_ATTR_IMPL(std::string, AttributeProto_AttributeType::AttributeProto_AttributeType_STRING, s)
ADD_ATTR_IMPL(TensorProto, AttributeProto_AttributeType::AttributeProto_AttributeType_TENSOR, t)
ADD_ATTR_IMPL(GraphProto, AttributeProto_AttributeType::AttributeProto_AttributeType_GRAPH, g)
ADD_LIST_ATTR_IMPL(float, AttributeProto_AttributeType::AttributeProto_AttributeType_FLOATS, floats)
ADD_LIST_ATTR_IMPL(int64_t, AttributeProto_AttributeType::AttributeProto_AttributeType_INTS, ints)
ADD_LIST_ATTR_IMPL(std::string, AttributeProto_AttributeType::AttributeProto_AttributeType_STRINGS, strings)
ADD_LIST_ATTR_IMPL(TensorProto, AttributeProto_AttributeType::AttributeProto_AttributeType_TENSORS, tensors)
ADD_LIST_ATTR_IMPL(GraphProto, AttributeProto_AttributeType::AttributeProto_AttributeType_GRAPHS, graphs)

bool Node::ClearAttribute(const std::string& attr_name)
{
    graph_->SetGraphResolveNeeded();
    graph_->SetGraphProtoSyncNeeded();
    return attributes_.erase(attr_name) > 0;
}

Status Node::UpdateInputArgCount()
{
    // The node refers to a primitive operator.
    // Infer and verify node input arg type information.
    auto total_arg_count = std::accumulate(definitions_.input_arg_count.cbegin(),
                                           definitions_.input_arg_count.cend(), 0);

    if (total_arg_count != definitions_.input_defs.size())
    {
        Status status(StatusCategory::ONNX, FAIL,
                      "The sum of input arg count is not equal to size of input defs in node (" + name_ + ").");
        return status;
    }

    // op_ is always valid when this is called
    auto& op = *op_;

    // Verify size of node arg count is same as input number in
    // operator definition.
    if (op.inputs().size() != definitions_.input_arg_count.size())
    {
        // Adjust input arg count array with op definition
        // The adjustment will work as below,
        // In total, there're <total_arg_count> inputs, which
        // will be split as <1, 1, 1, 1, ... 1, x> or
        // <1, 1, 1, 1, ...1, 0, 0, ...0>. The final input
        // arg count array's element number will be the same
        // as op definition, and the sum of all elements will
        // be equal to <total_arg_count>.
        auto& input_arg_count = definitions_.input_arg_count;
        input_arg_count.clear();
        size_t m = 0;
        auto arg_count_left = total_arg_count;

        if (0 < op.inputs().size())
        {
            for (; m < op.inputs().size() - 1; ++m)
            {
                if (arg_count_left > 0)
                {
                    input_arg_count.push_back(1);
                    arg_count_left--;
                }
                else
                {
                    input_arg_count.push_back(0);
                }
            }
        }

        // Set the arg count for the last input formal parameter.
        // NOTE: in the case that there's no .input(...) defined
        // in op schema, all input args will be fed as one input
        // of the operator.
        input_arg_count.push_back(arg_count_left);

        graph_->SetGraphResolveNeeded();
        graph_->SetGraphProtoSyncNeeded();
    }

    return Status::OK();
}

const NodeAttributes& Node::GetAttributes() const noexcept
{
    return attributes_;
}

void Node::ForEachDef(std::function<void(const ONNXIR::NodeArg*, bool is_input)> func) const
{
    for (const ONNXIR::NodeArg* arg : InputDefs())
    {
        if (!arg->Exists())
            continue;
        func(&*arg, true);
    }
    for (const ONNXIR::NodeArg* arg : OutputDefs())
    {
        if (!arg->Exists())
            continue;
        func(&*arg, false);
    }
};

void Node::ForEachInputDef(std::function<void(const ONNXIR::NodeArg*)> func) const
{
    for (const ONNXIR::NodeArg* arg : InputDefs())
    {
        if (!arg->Exists())
            continue;
        func(&*arg);
    }
};

void Node::ForEachOutputDef(std::function<void(const ONNXIR::NodeArg*)> func) const
{
    for (const ONNXIR::NodeArg* arg : OutputDefs())
    {
        if (!arg->Exists())
            continue;
        func(&*arg);
    }
};

void Node::ReplaceDefs(const std::map<ONNXIR::NodeArg*, ONNXIR::NodeArg*>& replacements)
{
    std::vector<std::vector<NodeArg*>*> all_defs = {&definitions_.input_defs, &definitions_.output_defs};

    for (auto pair : replacements)
        for (auto defs : all_defs)
            for (size_t i = 0, end = defs->size(); i < end; ++i)
                if (defs->at(i) == pair.first)
                    defs->at(i) = pair.second;
}

// Constructor: Given a <GraphProto> loaded from model file, construct
// a <Graph> object and Resolve() it.
//Status Graph::LoadGraph(const GraphProto& graph_proto,
//                        const std::unordered_map<std::string, int>& domain_to_version,
//                        Version ir_version,
//                        std::unique_ptr<Graph>& new_graph) {
//  // create instance. need to call private ctor so can't use make_unique
//  GSL_SUPPRESS(r .11)
//  new_graph.reset(new Graph(nullptr, &graph_proto, domain_to_version, ir_version));
//
//  // as we just loaded from file we want to fully initialize/Resolve, but not let that change
//  // the proto sync flag
//  auto status = new_graph->Resolve(/* no_proto_sync_required */ true);
//  return status;
//}

Graph::Graph(GraphProto* graph_proto,
             const std::unordered_map<std::string, int>& domain_to_version,
             Version ir_version,
             const LotusOpSchemaRegistry* local_registry)

    : GraphBase(/* resolve needed */ true, /* proto sync needed */ false, domain_to_version, ir_version),
      graph_proto_{graph_proto},
      graph_type_{Type::Main},
      local_registry_(local_registry)
{
    LOTUS_ENFORCE(graph_proto != nullptr, "Expected either name or graph_proto.");
    ArgNameToTypeMap name_to_type_map;

    // these are all empty unless we received a graph_proto as input
    if (graph_proto != nullptr)
    {
        // Copy initial tensors to a map.
        for (auto& tensor : graph_proto_->initializer())
        {
            name_to_initial_tensor_[tensor.name()] = &tensor;
        }

        // Collect all node arg name, type, shape information in the graph.
        // type/shape information will be assigned to each node arg when going
        // thru all nodes later.
        for (auto& graph_input : graph_proto_->input())
        {
            if (graph_input.has_name() && graph_input.has_type())
            {
                name_to_type_map[graph_input.name()] = graph_input.type();
            }
        }

        for (auto& graph_output : graph_proto_->output())
        {
            if (graph_output.has_name() && graph_output.has_type())
            {
                name_to_type_map[graph_output.name()] = graph_output.type();
            }
        }

        for (auto& node_arg : graph_proto_->value_info())
        {
            if (node_arg.has_name() && node_arg.has_type())
            {
                name_to_type_map[node_arg.name()] = node_arg.type();
            }
        }
    }

    // Add nodes.
    AddSourceSinkNodes();

    for (auto node_proto : graph_proto_->node())
    {
        AddNode(node_proto, name_to_type_map);
    }
}

Status GraphBase::VerifyNoDuplicateName(/*out*/ std::unordered_map<std::string, Node*>& output_args,
                                        /*out*/ std::unordered_map<std::string, NodeIndex>& node_name_to_index)
{
    output_args.clear();
    node_name_to_index.clear();

    for (auto& node : Nodes())
    {
        // Verify node name should be unique.
        auto& node_name = node.Name();

        if (!node_name.empty() && node_name_to_index.end() != node_name_to_index.find(node_name))
        {
            // The node has name and its name was used by another node.
            Status status(StatusCategory::ONNX, FAIL,
                          "Error: two nodes with same node name (" + node_name + ").");
            return status;
        }

        node_name_to_index[node_name] = node.Index();

        // Verify node outputs' name should be unique.
        for (const NodeArg* output_def : node.OutputDefs())
        {
            auto& output_arg_name = output_def->Name();
            auto result = output_args.insert({output_arg_name, &node});
            if (!result.second)
            {
                // Two outputs with same name, so that insertion fails.
                Status status(StatusCategory::ONNX, FAIL,
                              "Error: two output args with same name (" + output_arg_name + ").");
                return status;
            }
        }
    }
    return Status::OK();
}

GSL_SUPPRESS(es .84) // ignoring return value from unordered_map::insert causes noisy complaint
Status GraphBase::BuildConnections(const std::unordered_map<std::string, Node*>& output_args,
                                   const std::unordered_map<std::string, NodeIndex>& node_name_to_index)
{
    std::unordered_set<Node*> inner_nodes;

    for (auto& node : Nodes())
    {
        if (IsSourceNode(node) || IsSinkNode(node))
        {
            continue;
        }

        for (auto& control_input : node.ControlInputs())
        {
            auto name_to_index_iter = node_name_to_index.find(control_input);
            if (node_name_to_index.end() == name_to_index_iter)
            {
                Status status(StatusCategory::ONNX, FAIL,
                              "The control input (" + control_input + ") of Node (" +
                                  node.Name() + ") does not exist in the graph.");
                return status;
            }

            const NodeIndex src_node_index = name_to_index_iter->second;
            const NodeIndex dst_node_index = node.Index();
            auto dst = GetNode(dst_node_index);
            auto src = GetNode(src_node_index);
            LOTUS_ENFORCE(dst && src, "ControlInputs should not have invalid nodes. dst=", dst, " src=", src);
            src->MutableRelationships().output_nodes.insert(dst);
            dst->MutableRelationships().input_nodes.insert(src);
        }

        auto& input_args = node.InputDefs();
        if (input_args.size() > 0)
        {
            // This node needs inputs.

            for (const NodeArg* input_arg : input_args)
            {
                if (!input_arg->Exists())
                {
                    // This input could be optional and it does not exist in this case.
                    continue;
                }

                auto output_arg_iter = output_args.find(input_arg->Name());
                if (output_args.end() == output_arg_iter)
                {
                    // No such output_arg matching this input_arg.
                    // This input arg should be fed when running evaluation.

                    // Add a control edge between <souce> node and this node.
                    NO_CHANGE_ON_SYNC_FLAG(AddControlEdge(source_node_index_, node.Index()));
                    continue;
                }

                // Setup input/output relationship between <*node_iter>
                // and <output_arg_iter>.
                Node& output_node = *output_arg_iter->second;

                node.MutableRelationships().input_nodes.insert(&output_node);

                auto new_edge = std::make_unique<Node::EdgeEnd>(output_node, *input_arg);
                node.MutableRelationships().input_edges.insert(new_edge.get());
                owned_edges_.push_back(std::move(new_edge));

                output_node.MutableRelationships().output_nodes.insert(&node);

                new_edge = std::make_unique<Node::EdgeEnd>(node, *input_arg);
                output_node.MutableRelationships().output_edges.insert(new_edge.get());
                owned_edges_.push_back(std::move(new_edge));

                inner_nodes.insert(&output_node);
            }
        }
        else
        {
            if (node.OutputDefs().size() <= 0)
            {
                // This is a useless node.
                // It has no input/output.
                RemoveNode(node.Index());
            }

            // This is a starting node.
            // Add a control edge between <souce> node and this node.
            NO_CHANGE_ON_SYNC_FLAG(AddControlEdge(source_node_index_, node.Index()));
        }
    }

    for (auto& node : Nodes())
    {
        if (IsSourceNode(node) || IsSinkNode(node))
        {
            continue;
        }

        if (inner_nodes.size() <= 0 || inner_nodes.end() == inner_nodes.find(&node))
        {
            // This is an ending node.
            // Add a control edge from this node to sink node.
            NO_CHANGE_ON_SYNC_FLAG(AddControlEdge(node.Index(), sink_node_index_));
        }
    }

    return Status::OK();
}

void GraphBase::ReverseDFSFrom(const std::vector<NodeIndex>& from,
                               const std::function<void(const Node*)>& enter,
                               const std::function<void(const Node*)>& leave,
                               const std::function<bool(const Node*, const Node*)>& comp) const
{
    std::vector<const Node*> node_vec;
    for (auto i : from)
    {
        node_vec.push_back(GetNode(i));
    }

    ReverseDFSFrom(node_vec, enter, leave, comp);
}

void GraphBase::ReverseDFSFrom(const std::vector<const Node*>& from,
                               const std::function<void(const Node*)>& enter,
                               const std::function<void(const Node*)>& leave,
                               const std::function<bool(const Node*, const Node*)>& comp) const
{
    using WorkEntry = std::pair<const Node*, bool>; // bool represents leave or not
    std::vector<WorkEntry> stack(from.size());
    for (size_t i = 0; i < from.size(); i++)
    {
        stack[i] = WorkEntry(from[i], false);
    }

    std::vector<bool> visited(MaxNodeIndex(), false);
    while (!stack.empty())
    {
        const WorkEntry last_entry = stack.back();
        stack.pop_back();
        const Node& n = *last_entry.first;
        if (last_entry.second)
        {
            // leave node
            leave(&n);
            continue;
        }

        if (visited[n.Index()])
            continue;

        visited[n.Index()] = true;

        if (enter)
            enter(&n);

        if (leave)
            stack.push_back(WorkEntry(&n, true));

        if (comp)
        {
            std::vector<const Node*> sorted_nodes;
            for (auto iter = n.InputNodesBegin(); iter != n.InputNodesEnd(); ++iter)
            {
                sorted_nodes.push_back((*iter));
            }
            std::sort(sorted_nodes.begin(), sorted_nodes.end(), comp);
            for (const ONNXIR::Node* in : sorted_nodes)
            {
                const NodeIndex idx = in->Index();
                if (!visited[idx])
                {
                    stack.push_back(WorkEntry(in, false));
                }
            }
        }
        else
        {
            for (auto iter = n.InputNodesBegin(); iter != n.InputNodesEnd(); ++iter)
            {
                const NodeIndex idx = (*iter)->Index();
                if (!visited[idx])
                {
                    stack.push_back(WorkEntry(GetNode(idx), false));
                }
            }
        }
    }
}

GSL_SUPPRESS(es .84) // noisy warning about ignoring return value from insert(...)
Status GraphBase::CheckIsAcyclic(std::vector<NodeIndex>& nodes_in_topological_order) const
{
    nodes_in_topological_order.clear();
    // nodes that have been processed and added to nodes_in_topological_order.
    std::unordered_set<NodeIndex> visited_nodes;
    std::unordered_set<NodeIndex> ancestor_nodes;
    // tracks nodes whose child nodes have been processed.
    std::unordered_set<NodeIndex> children_visited_nodes;
    std::stack<NodeIndex> stack;
    stack.push(sink_node_index_);

    while (!stack.empty())
    {
        const NodeIndex current = stack.top();
        stack.pop();

        if (visited_nodes.end() != visited_nodes.find(current))
        {
            // The node has been visited before
            continue;
        }

        if (children_visited_nodes.end() != children_visited_nodes.find(current))
        {
            // children are done so we mark this one complete.
            visited_nodes.insert(current);
            nodes_in_topological_order.push_back(current);
            ancestor_nodes.erase(current);
            continue;
        }

        const Node* node = GetNode(current);
        LOTUS_ENFORCE(node != nullptr, "Invalid null entry in current graph.");

        if (node->InputNodesBegin() == node->InputNodesEnd())
        {
            // no children
            children_visited_nodes.insert(current);
            visited_nodes.insert(current);
            nodes_in_topological_order.push_back(current);
            ancestor_nodes.erase(current);
            continue;
        }

        stack.push(current);

        // mark as children done. by the time the node is popped off the stack again,
        // its children will have been processed
        children_visited_nodes.insert(current);

        ancestor_nodes.insert(current);

        // check children
        for (auto iter = node->InputNodesBegin(); iter != node->InputNodesEnd(); ++iter)
        {
            const NodeIndex idx = (*iter)->Index();
            if (ancestor_nodes.end() != ancestor_nodes.find(idx))
            {
                Status status(StatusCategory::ONNX, FAIL, "Error: the graph is not acyclic.");
                return status;
            }

            // avoid re-processing nodes
            if (children_visited_nodes.end() == children_visited_nodes.find(idx))
            {
                stack.push(idx);
            }
        }
    }

    if (NumberOfNodes() == nodes_in_topological_order.size())
    {
        return Status::OK();
    }
    else
    {
        return Status(StatusCategory::ONNX, FAIL, "Error: the graph is not acyclic.");
    }
}

bool FullyDefinedType(const TypeProto& type_proto)
{
    switch (type_proto.value_case())
    {
    case TypeProto::kTensorType:
    {
        auto& tensor_type = type_proto.tensor_type();
        return tensor_type.has_elem_type() && (tensor_type.elem_type() != TensorProto::UNDEFINED);
    }
    case TypeProto::kSequenceType:
    {
        auto& seq_type = type_proto.sequence_type();
        return seq_type.has_elem_type() && FullyDefinedType(seq_type.elem_type());
    }
    case TypeProto::kMapType:
    {
        auto& map_type = type_proto.map_type();
        return map_type.has_key_type() &&
               (map_type.key_type() != TensorProto::UNDEFINED) &&
               map_type.has_value_type() &&
               FullyDefinedType(map_type.value_type());
    }
    case TypeProto::VALUE_NOT_SET:
    default:
        return false;
    }
}

// An implementation of the InferenceContext interface required by operator-specific
// shape inference for Lotus graphs.
class InferenceContextImpl : public onnx::InferenceContext
{
public:
    InferenceContextImpl(Node& node, std::vector<TypeProto>& inferred_shapes) noexcept
        : node_(node),
          allOutputTypes_(inferred_shapes) {}

    const AttributeProto* getAttribute(const std::string& name) const override
    {
        auto& attribute_value_map = node_.GetAttributes();
        auto iter = attribute_value_map.find(name);
        if (iter == attribute_value_map.end())
        {
            return nullptr;
        }
        else
        {
            return &iter->second;
        }
    }

    size_t getNumInputs() const noexcept override
    {
        return node_.InputDefs().size();
    }

    const TypeProto* getInputType(size_t index) const override
    {
        auto p_node_arg = node_.InputDefs().at(index);
        if ((nullptr != p_node_arg) && p_node_arg->Exists())
        {
            return p_node_arg->TypeAsProto();
            // auto p_type_proto = p_node_arg->TypeAsProto();
            //if ((p_type_proto != nullptr) && p_type_proto->has_tensor_type()) {
            //  return &p_type_proto->tensor_type();
            //}
        }
        return nullptr;
    }

    size_t getNumOutputs() const noexcept override
    {
        return allOutputTypes_.size();
    }

    TypeProto* getOutputType(size_t index) override
    {
        return &allOutputTypes_[index];
    }

private:
    Node& node_;
    // allOutputTypes_ will be populated by the operator-specific shape inference.
    std::vector<TypeProto>& allOutputTypes_;
    // std::vector<TypeProto_Tensor>& allOutputTypes_;
};

// A wrapper for invoking ONNX-defined shape+type inference for a single node.
// Returns inferred shape+type for every output of the node in output parameter inferredShapes.
Status GraphBase::InferOutputTypesAndShapes(ONNXIR::Node& node, std::vector<TypeProto>& inferred_shapes)
{
    inferred_shapes.clear();
    inferred_shapes.resize(node.OutputDefs().size());
    auto schema = node.Op();
    if (nullptr != schema)
    {
        InferenceContextImpl context(node, inferred_shapes);
        schema->GetTypeAndShapeInferenceFunction()(context);
    }
    return Status::OK();
}

// Implementation of type-inference and type-checking for a single node
Status Graph::InferAndVerifyTypeMatch(Node& node,
                                      const OpSchema& op,
                                      const std::unordered_map<std::string, Node*>& output_args)
{
    auto& nodeName = node.Name();

    // <k> index used to navigate node->InputDefs().
    int k = 0;
    std::unordered_map<std::string, DataType> type_parameter_to_type_map;

    for (size_t i = 0; i < node.InputArgCount().size(); ++i)
    {
        // Number of inputs corresponding to the i-th argument.
        const int arg_count = node.InputArgCount()[i];
        // The i-th formal parameter definition.
        auto op_formal_parameter = op.inputs()[i];

        // Check all <arg_count> actual parameters (corresponding to the k-th input)
        // match the formal parameter definition (i-th argument).
        for (int j = 0; j < arg_count; ++j, ++k)
        {
            auto& input_def = node.MutableDefinitions().input_defs[k];
            if (!input_def->Exists())
                continue;

            // Determine the type of the actual input parameter:

            // Check if the actual input parameter is the output of some preceding node:
            auto output_args_iter = output_args.find(input_def->Name());
            if (output_args.end() == output_args_iter)
            {
                // An input parmeter that is not the output of a preceding node
                // should be a graph input (either fed by callers or in initializers).
                // The ONNX spec requires every initializer to be included in the graph input,
                // and that every graph input's type should be specified.
                //
                // TODO: We relax this requirement here and this requires some rethinking.
                auto initial_tensor_iter = name_to_initial_tensor_.find(input_def->Name());
                if (name_to_initial_tensor_.end() != initial_tensor_iter)
                {
                    // This input is fed with default value by initializer.
                    // Infer its type from initializer tensor.
                    TypeProto initial_tensor_type;
                    initial_tensor_type.mutable_tensor_type()->set_elem_type(
                        initial_tensor_iter->second->data_type());
                    input_def->SetType(DataTypeUtils::ToType(initial_tensor_type));

                    // Set shape accordingly.
                    TensorShapeProto shape;
                    for (auto dim : initial_tensor_iter->second->dims())
                    {
                        shape.add_dim()->set_dim_value(dim);
                    }
                    input_def->SetShape(shape);
                }
                else if (!input_def->ToProto().has_type())
                {
                    // This input is fed by callers and its type has to be specified.

                    Status status(StatusCategory::ONNX, FAIL,
                                  "Node (" + nodeName + ") input arg (" +
                                      input_def->Name() + ") does not have type information.");
                    return status;
                }
            }
            else
            {
                // The type of this input should have been set during type-inference of the
                // node that outputs this NodeArg
                if (input_def->Type() == nullptr)
                {
                    // Logic error: This should not happen.
                    Status status(StatusCategory::ONNX, FAIL,
                                  "Node (" + nodeName + ") input arg (" +
                                      input_def->Name() + ") does not have type information set by parent node.");
                    return status;
                }
            }

            // Verify that the actual parameter's type is one of permitted types of the formal parameter
            DataType input_type = input_def->Type();
            auto& permitted_types = op_formal_parameter.GetTypes();
            if (0 == permitted_types.count(input_type))
            {
                // Type error in input model/graph.
                Status status(StatusCategory::ONNX, FAIL,
                              "Type Error: Type of input parameter (" + input_def->Name() +
                                  ") of operator (" + op.Name() + ") in node (" + nodeName + ") is invalid.");
                return status;
            }

            // Check that type-parameters are bound to the same value:
            auto param_to_type_iter = type_parameter_to_type_map.find(op_formal_parameter.GetTypeStr());
            if (type_parameter_to_type_map.end() == param_to_type_iter)
            {
                // Bind the corresponding type-parameter's value to the actual type:
                type_parameter_to_type_map[op_formal_parameter.GetTypeStr()] = input_type;
            }
            else if (param_to_type_iter->second != input_type)
            {
                // Type error in input model/graph:
                // The type-parameter T is bound to different values for different inputs.
                // E.g., Add(A,B) where A is of type "tensor(int32)" and B is of type "tensor(float)".
                // NOTE: for variadic arguments, this verification rule is currently applicable:
                // e.g., Concat/Max/Mean/Min/Sum all require all input tensors to be of same type.
                // However, this will need to be extended to handle the If-Then-Else and Loop
                // constructs in future which will have variadic inputs and outputs of different types.

                Status status(StatusCategory::ONNX, FAIL,
                              "Type Error: Type parameter (" + op_formal_parameter.GetTypeStr() +
                                  ") bound to different types (" + *(param_to_type_iter->second) +
                                  " and " + *(input_def->Type()) +
                                  " in node (" + nodeName + ").");
                return status;
            }
        }
    }

    // Apply ONNX's shape/type inference to node
    std::vector<TypeProto> onnx_inferred_types;
    try
    {
        LOTUS_RETURN_IF_ERROR(InferOutputTypesAndShapes(node, onnx_inferred_types));
    }
    catch (const std::exception& ex)
    {
        return Status(StatusCategory::ONNX, FAIL, ex.what());
    }

    // Infer and verify node output arg type information.
    int i = 0;
    for (auto& output_def : node.MutableDefinitions().output_defs)
    {
        if (!output_def->Exists())
            continue;

        // if the number of actual parameters exceeds the number of formal parameters,
        // then the op has variadic outputs and the trailing extra actual parameters
        // correspond to the last formal parameter. (The ONNX schema verification check
        // would have checked that the corresponding formal parameter is variadic.)

        const int num_formal_params = gsl::narrow_cast<int>(op.outputs().size());
        auto operand_index = std::min(i, num_formal_params - 1);
        auto op_formal_parameter = op.outputs().at(operand_index);

        const TypeProto& onnx_inferred_type = onnx_inferred_types[i++];
        DataType existing_type = output_def->Type();
        DataType inferred_type = nullptr;

        // Infer output arg type if it is constrained to be of the same type as some input:
        // For example, the output of "Abs" is of the same type as its input.
        auto input_types_iter = type_parameter_to_type_map.find(op_formal_parameter.GetTypeStr());
        if (type_parameter_to_type_map.end() != input_types_iter)
        {
            inferred_type = input_types_iter->second;
        }
        else if (1 == op_formal_parameter.GetTypes().size())
        {
            // Infer output arg type if operator definition specifies unique output type:
            inferred_type = *(op_formal_parameter.GetTypes().begin());
        }
        else if (FullyDefinedType(onnx_inferred_type))
        {
            // Use output type inferred by ONNX inference
            inferred_type = DataTypeUtils::ToType(onnx_inferred_type);
        }
        else if (existing_type != nullptr)
        {
            inferred_type = existing_type;
        }
        else
        {
            // This should not happen: indicates incompleteness in ONNX inference.
            Status status(StatusCategory::ONNX, FAIL,
                          "Node (" + nodeName + ") output arg (" + output_def->Name() + ") type inference failed");
            return status;
        }

        if ((existing_type != inferred_type) && (existing_type != nullptr))
        {
            // A type exists for this output but does not match the inferred type.
            return Status(StatusCategory::ONNX, FAIL,
                          "Type Error: Type (" + *existing_type + ") of output arg (" +
                              output_def->Name() + ") of node (" + nodeName +
                              ") does not match expected type (" + *inferred_type + ").");
        }

        output_def->SetType(inferred_type);

        // Update output-shape if it was inferred:
        if (onnx_inferred_type.has_tensor_type())
        {
            auto& tensor_type = onnx_inferred_type.tensor_type();
            if (tensor_type.has_shape())
            {
                output_def->SetShape(tensor_type.shape());
            }
        }
    }

    return Status::OK();
} // namespace ONNXIR

#define enforce_non_empty_field(proto, field)     \
    do                                            \
    {                                             \
        if (proto.field().empty())                \
        {                                         \
            fail_check(                           \
                "Field '",                        \
                #field,                           \
                "' of ",                          \
                #proto,                           \
                " is required to be non-empty."); \
        }                                         \
    } while (0)

static void check_node(
    const NodeProto& node,
    const CheckerContext& ctx,
    const LexicalScopeContext& lex_ctx,
    const LotusOpSchemaRegistry* registry)
{
    enforce_non_empty_field(node, op_type);

    if (node.input().empty() && node.output().empty())
    {
        fail_check(
            "NodeProto (name: ",
            node.name(),
            ", type: ",
            node.op_type(),
            ") has zero input and zero output.");
    }

    // Resolve domain for node
    const auto& opset_imports = ctx.get_opset_imports();
    auto dit = opset_imports.find(node.domain());
    if (dit == opset_imports.end())
    {
        fail_check("No opset import for domain '" + node.domain() + "'");
    }
    auto domain_version = dit->second;

    for (const auto& attr : node.attribute())
    {
        check_attribute(attr, ctx, lex_ctx);
    }

    const auto* schema =
        registry->Schema(node.op_type(), domain_version, node.domain());
    if (!schema)
    {
        fail_check(
            "No Schema registered for " + node.op_type() +
            " with domain_version of " + ONNX_NAMESPACE::to_string(domain_version));
    }
    schema->Verify(node);
}

Status Graph::VerifyNodeAndOpMatch(const std::vector<NodeIndex>& nodes_in_topological_order,
                                   const std::unordered_map<std::string, Node*>& output_args)
{
    for (auto nodeIndex : nodes_in_topological_order)
    {
        if (IsSourceNode(nodeIndex) || IsSinkNode(nodeIndex))
        {
            continue;
        }
        // Node verification.
        auto& node = *GetNode(nodeIndex);
        CheckerContext ctx;
        ctx.set_ir_version(gsl::narrow_cast<int>(IrVersion()));
        ctx.set_opset_imports(DomainToVersionMap());
        LexicalScopeContext lsc;
        for (auto& kv : output_args)
        {
            auto ignored = lsc.output_names.insert(kv.first);
        }
        NodeProto node_proto;
        node.ToProto(node_proto);
        auto& node_name = node.Name();
        auto& domain = node.Domain();
        auto maxInclusiveVersion = DomainToVersionMap().find(domain)->second;

        // check node from local schema first
        if (local_registry_)
        {
            try
            {
                check_node(node_proto, ctx, lsc, local_registry_);
                node.op_ = local_registry_->Schema(node.OpType(), maxInclusiveVersion, node.Domain());
            }
            catch (const std::exception& ex)
            {
                LOGS_DEFAULT(WARNING) << "verify node from local registry failed: " << ex.what() << std::endl
                                      << "Will try to search from build in schema." << std::endl;
            }
        }

        if (!node.op_)
        {
            try
            {
                checker::check_node(node_proto, ctx, lsc);
            }
            catch (const std::exception& ex)
            {
                return Status(StatusCategory::ONNX, FAIL, ex.what());
            }
            node.op_ = OpSchemaRegistry::Schema(node.OpType(), maxInclusiveVersion, node.Domain());
        }

        RETURN_IF_ERROR(node.UpdateInputArgCount());

        // currently an Op is required by ValidateVersion, so we use gsl::not_null.
        // This may change in the future to allow a null Op
        const OpSchema* p_op = node.Op();

        // Attribute verification and fill node attribute with
        // default value defined in operator definition if needed.
        // Fill node attribute with default value specified in operator definition if any.
        auto node_attributes = node.GetAttributes();
        for (auto attr_def : p_op->attributes())
        {
            auto node_attr_iter = node_attributes.find(attr_def.first);
            if (node_attributes.end() == node_attr_iter)
            {
                // The attribute was not specified in the node.
                if (!attr_def.second.required)
                {
                    if (attr_def.second.default_value.has_name())
                    {
                        // Set default value to the node attributes.
                        node.AddAttribute(attr_def.first, attr_def.second.default_value);
                    }
                    // TODO: Handle optional attribute but no default value specified in op definition.
                }
                else
                {
                    Status status(StatusCategory::ONNX, FAIL,
                                  "Node (" + node_name + ") attribute (" + attr_def.first +
                                      ") is required but not specified.");
                    return status;
                }
            }
        }

        NO_CHANGE_ON_SYNC_FLAG(RETURN_IF_ERROR(InferAndVerifyTypeMatch(node, *p_op, output_args)));
    }

    return Status::OK();
}
Status Graph::Resolve()
{
    return Resolve(false);
}

Status Graph::Resolve(bool no_proto_sync_required)
{
    if (!GraphResolveNeeded())
    {
        return Status::OK();
    }

    std::unordered_map<std::string, Node*> output_args;
    std::unordered_map<std::string, NodeIndex> node_name_to_index;
    RETURN_IF_ERROR(VerifyNoDuplicateName(output_args, node_name_to_index));
    RETURN_IF_ERROR(BuildConnections(output_args, node_name_to_index));
    RETURN_IF_ERROR(CheckIsAcyclic(NodesInTopologicalOrder()));
    RETURN_IF_ERROR(VerifyNodeAndOpMatch(NodesInTopologicalOrder(), output_args));
    RETURN_IF_ERROR(SetGraphInputsOutputs());
    CleanInitializers();
    GraphResolveNeeded(false);

    // if we are resolving immediately after loading from a GraphProto, we don't need to
    // do a proto sync
    if (no_proto_sync_required)
    {
        GraphProtoSyncNeeded(false);
    }

    return Status::OK();
}

Status GraphBase::GetNodesInTopologicalOrder(const std::vector<NodeIndex>** pp_nodes) const
{
    if (graph_resolve_needed_)
    {
        return Status{StatusCategory::ONNX, StatusCode::FAIL,
                      "Resolve() must be called before using the graph as modifications have been made to it."};
    }

    *pp_nodes = &nodes_in_topological_order_;
    return Status::OK();
}

void GraphBase::AddSourceSinkNodes()
{
    std::vector<NodeArg*> empty_args;

    source_node_index_ = AddNode("_Graph_Source", kNoOp,
                                 "Source node internally in a graph.", empty_args, empty_args)
                             ->Index();

    sink_node_index_ = AddNode("_Graph_Sink", kNoOp,
                               "Sink node internally in a graph.", empty_args, empty_args)
                           ->Index();

    NO_CHANGE_ON_SYNC_FLAG(AddControlEdge(source_node_index_, sink_node_index_));
}

const std::string& Graph::Name() const noexcept
{
    return graph_proto_->name();
}

void Graph::SetName(const std::string& name)
{
    graph_proto_->set_name(name);
}

const std::string& Graph::Description() const noexcept
{
    return graph_proto_->doc_string();
}

void Graph::SetDescription(const std::string& description)
{
    graph_proto_->set_doc_string(description);
}

void Graph::AddInitializedTensor(const TensorProto& tensor)
{
    if (name_to_initial_tensor_.end() != name_to_initial_tensor_.find(tensor.name()))
    {
        return;
    }

    auto tensorAdded = graph_proto_->add_initializer();
    *(tensorAdded) = tensor;
    name_to_initial_tensorIndex_[tensor.name()] = graph_proto_->initializer_size() - 1;
    name_to_initial_tensor_[tensor.name()] = tensorAdded;

    SetGraphProtoSyncNeeded();
    SetGraphResolveNeeded();
}

void Graph::RemoveInitializedTensor(const std::string& tensor_name)
{
    auto iter = name_to_initial_tensorIndex_.find(tensor_name);
    if (name_to_initial_tensorIndex_.end() != iter)
    {
        removed_initializer_indexes_.push_back(iter->second);
        name_to_initial_tensorIndex_.erase(tensor_name);
        name_to_initial_tensor_.erase(tensor_name);
        SetGraphProtoSyncNeeded();
        SetGraphResolveNeeded();
    }
}

bool Graph::GetInitializedTensor(const std::string& tensor_name, const TensorProto** value) const
{
    auto iter = name_to_initial_tensor_.find(tensor_name);
    if (name_to_initial_tensor_.end() == iter)
    {
        return false;
    }
    *value = iter->second;
    return true;
}

void Graph::CleanAllInitializedTensors() noexcept
{
    name_to_initial_tensorIndex_.clear();
    name_to_initial_tensor_.clear();
    removed_initializer_indexes_.clear();

    // Clearing RepeatedPtrFields does not free objects' memory. The memory is retained
    // and can be reused. Need to explicitly release the cleared objects and free the
    // memory.
    graph_proto_->mutable_initializer()->Clear();
    int num_cleared = graph_proto_->initializer().ClearedCount();
    for (int i = 0; i < num_cleared; i++)
    {
        delete graph_proto_->mutable_initializer()->ReleaseCleared();
    }
}

const InitializedTensorSet& Graph::GetAllInitializedTensors() const noexcept
{
    return name_to_initial_tensor_;
}

const std::vector<const NodeArg*>& Graph::GetValueInfo() const noexcept
{
    return value_info_;
}

// Ensure the NodeArgs in the input are created and in this Graph's node arg map
static void AddNodeArgs(const std::vector<NodeArg*>& input_args,
                        std::unordered_map<std::string, NodeArg*>& node_arg_map)
{
    for (const NodeArg* input_arg : input_args)
    {
        auto& key = input_arg->Name();
        auto existing_entry = node_arg_map.find(key);

        NodeArg* node_arg = existing_entry == node_arg_map.end() ? nullptr : existing_entry->second;

        if (node_arg == nullptr)
        {
            node_arg_map[key] = node_arg;
        }
        else
        {
            // check that if an existing entry was found, it was for the same instance
            LOTUS_ENFORCE(node_arg == input_arg,
                          "Existing entry in NodeArg map for ", key, " != input definition.");
        }
    }
}

static std::vector<NodeArg*> CreateNodeArgs(const google::protobuf::RepeatedPtrField<std::string>& names,
                                            const ArgNameToTypeMap& name_to_type_map,
                                            std::unordered_map<std::string, NodeArg*>& node_arg_map,
                                            std::vector<std::unique_ptr<NodeArg>>& owned_node_args)
{
    const auto name_to_type_map_end = name_to_type_map.end();
    std::vector<NodeArg*> results;
    results.reserve(names.size());

    for (auto& name : names)
    {
        const TypeProto* type = nullptr;

        auto name_to_type_iter = name_to_type_map.find(name);
        if (name_to_type_iter != name_to_type_map_end)
        {
            // This node input arg type/shape does exist in graph proto.
            // Assign type/shape information to node input arg.
            type = &(name_to_type_iter->second);
        }

        auto existing_entry = node_arg_map.find(name);
        NodeArg* node_arg = existing_entry == node_arg_map.end() ? nullptr : existing_entry->second;

        if (node_arg == nullptr)
        {
            auto new_node_arg = std::make_unique<NodeArg>(name, type);
            node_arg = new_node_arg.get();
            owned_node_args.push_back(std::move(new_node_arg));
            node_arg_map[name] = node_arg;
        }

        results.push_back(node_arg);
    }

    return results;
}

Node* GraphBase::AddNode(const Node& other)
{
    const auto& definitions = other.GetDefinitions();

    auto new_node = AddNode(other.Name(), other.OpType(), other.Description(),
                            definitions.input_defs,
                            definitions.output_defs,
                            &other.GetAttributes(),
                            other.Domain());

    return new_node;
}

Node* GraphBase::AddNode(const NodeProto& node_proto,
                         const ArgNameToTypeMap& name_to_type_map)
{
    Node* node = AllocateNode();

    auto input_defs = CreateNodeArgs(node_proto.input(), name_to_type_map, node_args_, owned_node_args_);
    auto output_defs = CreateNodeArgs(node_proto.output(), name_to_type_map, node_args_, owned_node_args_);

    const int num_attributes = node_proto.attribute_size();
    NodeAttributes attributes;
    attributes.reserve(num_attributes);

    for (int i = 0; i < num_attributes; ++i)
    {
        auto& attr = node_proto.attribute(i);
        attributes[attr.name()] = attr;
    }

    node->Init(node_proto.name(),
               node_proto.op_type(),
               node_proto.doc_string(),
               input_defs,
               output_defs,
               &attributes,
               node_proto.domain());

    return node;
}

Node* GraphBase::AddNode(const std::string& name,
                         const std::string& op_type,
                         const std::string& description,
                         const std::vector<NodeArg*>& input_args,
                         const std::vector<NodeArg*>& output_args,
                         const NodeAttributes* attributes,
                         const std::string& domain)
{
    AddNodeArgs(input_args, node_args_);
    AddNodeArgs(output_args, node_args_);

    Node* node = AllocateNode();
    node->Init(name, op_type, description, input_args, output_args, attributes, domain);
    if (0 != op_type.compare(kNoOp))
    {
        graph_proto_sync_needed_ = true;
    }

    return node;
}

bool GraphBase::RemoveNode(NodeIndex p_index)
{
    return ReleaseNode(p_index);
}

Node* GraphBase::AddConstantNode(const std::string& name,
                                 const std::string& description,
                                 const std::vector<NodeArg*>& output_args,
                                 const TensorProto& tensor)
{
    Node* node = AddNode(name, kConstant, description, std::vector<NodeArg*>{}, output_args);
    node->AddAttribute(kConstantValue, tensor);
    return node;
}

bool GraphBase::AddControlEdge(NodeIndex src_node_index, NodeIndex dst_node_index)
{
    if (nodes_.size() <= src_node_index ||
        nodes_.size() <= dst_node_index ||
        nullptr == nodes_[src_node_index] ||
        nullptr == nodes_[dst_node_index])
    {
        // Invalid node indexes specified.
        return false;
    }

    GSL_SUPPRESS(es .84)
    { // ignoring return from insert()
        nodes_[src_node_index]->MutableRelationships().output_nodes.insert(nodes_[dst_node_index].get());
        nodes_[dst_node_index]->MutableRelationships().input_nodes.insert(nodes_[src_node_index].get());
        nodes_[dst_node_index]->MutableRelationships().control_inputs.insert(nodes_[src_node_index]->Name());
    }

    return true;
}

const GraphProto& Graph::ToGraphProto()
{
    if (!GraphProtoSyncNeeded())
    {
        return *graph_proto_;
    }

    // Nodes.
    graph_proto_->clear_node();

    // Nodes must be sorted in Topological Order in the GraphProto per ONNX spec.
    for (auto& node_idx : NodesInTopologicalOrder())
    {
        if (IsSourceNode(node_idx) || IsSinkNode(node_idx))
        {
            continue;
        }

        NodeProto* node_proto = graph_proto_->add_node();
        Node* p_node = GetNode(node_idx);
        p_node->ToProto(*node_proto);
    }

    if (removed_initializer_indexes_.size() > 0)
    {
        // Move initializers.
        std::sort(removed_initializer_indexes_.begin(), removed_initializer_indexes_.end());
        int lastInUseInitializerIndex = graph_proto_->initializer_size() - 1;
        int start = 0, end = static_cast<int>(removed_initializer_indexes_.size()) - 1;
        int lastRemovedInitializerIndex = removed_initializer_indexes_[end];

        for (; start <= end; start++)
        {
            // Find a lastInUseInitializer.
            while (start <= end && lastInUseInitializerIndex == lastRemovedInitializerIndex)
            {
                graph_proto_->mutable_initializer()->RemoveLast();
                lastInUseInitializerIndex--;
                end--;
                if (start <= end)
                {
                    lastRemovedInitializerIndex = removed_initializer_indexes_[end];
                }
            }

            if (start <= end)
            {
                // Copy the <lastInUseInitializerIndex> initializer in use to the <start> slot which is removed.
                *graph_proto_->mutable_initializer(removed_initializer_indexes_[start]) = graph_proto_->initializer(lastInUseInitializerIndex);
                graph_proto_->mutable_initializer()->RemoveLast();
                lastInUseInitializerIndex--;
            }
        }
        removed_initializer_indexes_.clear();
    }

    // Sync graph inputs/outputs/valueInfo.
    SyncGraphInputsOutputs();

    GraphProtoSyncNeeded(false);

    return *graph_proto_;
}

void Graph::SyncGraphInputsOutputs()
{
    graph_proto_->clear_input();
    graph_proto_->clear_output();
    graph_proto_->clear_value_info();

    for (const ONNXIR::NodeArg* input_arg : GetInputs())
    {
        *(graph_proto_->mutable_input()->Add()) = input_arg->ToProto();
    }

    for (const ONNXIR::NodeArg* output_arg : GetOutputs())
    {
        *(graph_proto_->mutable_output()->Add()) = output_arg->ToProto();
    }

    for (const ONNXIR::NodeArg* value_info : value_info_)
    {
        *(graph_proto_->mutable_value_info()->Add()) = value_info->ToProto();
    }
}

void Graph::CleanInitializers()
{
    std::vector<std::string> wrong_names;
    const auto& inputs = GetInputs();

    for (const auto& pv : name_to_initial_tensor_)
    {
        const std::string& s = pv.first;
        if (std::none_of(inputs.begin(), inputs.end(),
                         [&s](const NodeArg* input) noexcept->bool { return s == input->Name(); }))
        {
            wrong_names.push_back(s);
        }
    }

    for (const std::string& s : wrong_names)
    {
        LOGF_DEFAULT(WARNING, "%s exists in this graph's initializers but it is not in its input list", s.c_str());
        name_to_initial_tensor_.erase(s);
    }
}

GSL_SUPPRESS(es .84) // warning about ignoring return value from insert(...)
Status Graph::SetGraphInputsOutputs()
{
    // Reset graphInputs/graphOutputs/valueInfo state.
    auto& graph_inputs = MutableInputs();
    auto& graph_outputs = MutableOutputs();

    graph_inputs.clear();
    graph_outputs.clear();
    value_info_.clear();

    // Flag indicates that this graph is loaded from model file.
    // If it's true, then graph inputs and outputs will keep the same
    // as what are specified in the model, otherwise, graph inputs
    // and outputs will be inferred.
    const bool loaded_from_model_file = graph_proto_->input_size() != 0 ||
                                        graph_proto_->output_size() != 0 ||
                                        graph_proto_->value_info_size() != 0;

    std::unordered_set<std::string> added_input_names{};

    if (loaded_from_model_file)
    {
        // Collect all graph inputs/outputs specified in original graph proto
        std::unordered_set<std::string> specified_graph_inputs;
        std::unordered_set<std::string> specified_graph_outputs;
        std::unordered_set<std::string> specified_graph_value_info;
        std::unordered_set<std::string> specified_initializers;

        for (auto& graph_input : graph_proto_->input())
        {
            specified_graph_inputs.insert(graph_input.name());
        }

        for (auto& graph_output : graph_proto_->output())
        {
            specified_graph_outputs.insert(graph_output.name());
        }

        for (auto& graph_value_info : graph_proto_->value_info())
        {
            specified_graph_value_info.insert(graph_value_info.name());
        }

        for (auto& initializer : graph_proto_->initializer())
        {
            specified_initializers.insert(initializer.name());
        }

        std::unordered_map<std::string, const NodeArg*> output_name_to_node_arg;
        for (const auto& node : Nodes())
        {
            for (const NodeArg* output_def : node.OutputDefs())
            {
                if (specified_graph_outputs.erase(output_def->Name()) >= 1)
                {
                    graph_outputs.push_back(output_def);
                }
                output_name_to_node_arg.insert({output_def->Name(), output_def});
            }
        }

        if (specified_graph_outputs.size() != 0)
        {
            return Status(StatusCategory::ONNX, FAIL, "Some graph outputs which don't exist in the graph.");
        }

        for (const auto& node : Nodes())
        {
            // Go thru all node's inputs.
            for (const NodeArg* input_arg : node.InputDefs())
            {
                if (!input_arg->Exists())
                {
                    // It's an optional input and does not exist in this case.
                    continue;
                }

                if (specified_graph_inputs.end() != specified_graph_inputs.find(input_arg->Name()))
                {
                    if (added_input_names.insert(input_arg->Name()).second)
                    {
                        // The node input is specified as graph input.
                        graph_inputs.push_back(input_arg);
                    }
                    continue;
                }

                auto output_arg_iter = output_name_to_node_arg.find(input_arg->Name());
                if (output_name_to_node_arg.end() == output_arg_iter &&
                    specified_initializers.end() == specified_initializers.find(input_arg->Name()))
                {
                    // The node input is not specified as graph input,
                    // and it's not fed by another node neither.
                    return Status(StatusCategory::ONNX, FAIL, "Node input (" + input_arg->Name() + ") should be a graph input.");
                }

                if (specified_graph_value_info.erase(input_arg->Name()) >= 1)
                {
                    value_info_.push_back(input_arg);
                }
            }
        }
    }
    else
    {
        std::unordered_map<std::string, const NodeArg*> output_name_to_node_arg;
        for (const auto& node : Nodes())
        {
            for (const NodeArg* output_def : node.OutputDefs())
            {
                output_name_to_node_arg.insert({output_def->Name(), output_def});
            }
        }

        // Init graph output args with all node output args.
        auto graph_output_args = output_name_to_node_arg;

        std::unordered_set<Node*> inner_nodes;
        for (const auto& node : Nodes())
        {
            // Go thru all node's inputs.
            for (const NodeArg* input_arg : node.InputDefs())
            {
                if (!input_arg->Exists())
                {
                    // It's an optional input and does not exist in this case.
                    continue;
                }

                auto output_arg_iter = output_name_to_node_arg.find(input_arg->Name());
                if (output_name_to_node_arg.end() == output_arg_iter)
                {
                    // This input arg should be fed when running evaluation.
                    // it should be a graph input.
                    if (added_input_names.end() == added_input_names.find(input_arg->Name()))
                    {
                        // This graph input has not been added into <graph_inputs_>.
                        graph_inputs.push_back(input_arg);
                        added_input_names.insert(input_arg->Name());
                    }
                }
                else if (graph_output_args.erase(output_arg_iter->first) >= 1)
                {
                    // Remove the output arg name from graph outputs since it's
                    // the input of another node, which we call it intermediate result
                    // and store it in <m_valueinfo>.
                    value_info_.push_back(input_arg);
                }
            }
        }

        // Set graph outputs.
        for (auto& output_arg : graph_output_args)
        {
            graph_outputs.push_back(output_arg.second);
        }
    }

    return Status::OK();
}

bool GraphBase::IsSourceNode(NodeIndex index) const noexcept
{
    return source_node_index_ == index;
}

bool GraphBase::IsSinkNode(NodeIndex index) const noexcept
{
    return sink_node_index_ == index;
}

const Node* GraphBase::SourceNode() const
{
    return nodes_[source_node_index_].get();
}

const Node* GraphBase::SinkNode() const
{
    return nodes_[sink_node_index_].get();
}

// calling private ctor
GSL_SUPPRESS(r .11)
Node* GraphBase::AllocateNode()
{
    std::unique_ptr<Node> new_node(new Node(nodes_.size(), *this));
    Node* node{new_node.get()};

    nodes_.push_back(std::move(new_node));
    ++num_of_nodes_;
    graph_resolve_needed_ = true;

    return node;
}

// TODO: Does this need (and maybe AllocateNode) to be threadsafe so nodes_ and num_of_nodes_ managed more carefully?
bool GraphBase::ReleaseNode(NodeIndex index)
{
    if (index >= nodes_.size())
    {
        return false;
    }

    // index is valid, but the entry may already be empty
    if (nodes_[index] != nullptr)
    {
        nodes_[index] = nullptr;
        --num_of_nodes_;
        graph_proto_sync_needed_ = true;
        graph_resolve_needed_ = true;
    }

    return true;
}
} // namespace ONNXIR
