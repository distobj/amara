#include "domlette_interface.h"

/** Private Routines **************************************************/

static PyObject *newobj_function;
static PyObject *shared_empty_nodelist;
static PyObject *xml_base_key;
static PyObject *is_absolute_function;
static PyObject *absolutize_function;

#define ContainerNode_SET_COUNT(op, v) (ContainerNode_GET_COUNT(op) = (v))
#define ContainerNode_GET_NODES(op) (((ContainerNodeObject *)(op))->nodes)
#define ContainerNode_SET_NODES(op, v) (ContainerNode_GET_NODES(op) = (v))
#define ContainerNode_GET_ALLOCATED(op)         \
  (((ContainerNodeObject *)(op))->allocated)
#define ContainerNode_SET_ALLOCATED(op, v)      \
  (ContainerNode_GET_ALLOCATED(op) = (v))
#define ContainerNode_GET_CHILD(op, i)          \
  (((ContainerNodeObject *)(op))->nodes[i])
#define ContainerNode_SET_CHILD(op, i, v)       \
  (ContainerNode_GET_CHILD((op), (i)) = (v))

Py_LOCAL_INLINE(int)
node_resize(ContainerNodeObject *self, Py_ssize_t newsize) {
  NodeObject **nodes;
  size_t new_allocated;
  Py_ssize_t allocated = self->allocated;

  /* Bypass realloc() when a previous overallocation is large enough
     to accommodate the newsize.  If the newsize falls lower than half
     the allocated size, then proceed with the realloc() to shrink the list.
  */
  if (allocated >= newsize && newsize >= (allocated >> 1)) {
    self->count = newsize;
    return 0;
  }

  /* This over-allocates proportional to the list size, making room
   * for additional growth.  The over-allocation is mild, but is
   * enough to give linear-time amortized behavior over a long
   * sequence of appends() in the presence of a poorly-performing
   * system realloc().
   * The growth pattern is:  0, 4, 8, 16, 25, 35, 46, 58, 72, 88, ...
   */
  new_allocated = (newsize >> 3) + (newsize < 9 ? 3 : 6) + newsize;
  if (newsize == 0)
    new_allocated = 0;
  nodes = self->nodes;
  if (new_allocated <= ((~(size_t)0) / sizeof(NodeObject *)))
    PyMem_Resize(nodes, NodeObject *, new_allocated);
  else
    nodes = NULL;
  if (nodes == NULL) {
    PyErr_NoMemory();
    return -1;
  }
  self->nodes = nodes;
  self->count = newsize;
  self->allocated = new_allocated;
  return 0;
}

Py_LOCAL_INLINE(int)
node_validate_child(NodeObject *self, NodeObject *child)
{
  if (self == NULL || child == NULL || !Node_Check(self)) {
    PyErr_BadInternalCall();
    return 0;
  } else if (!Node_HasFlag(self, Node_FLAGS_CONTAINER)) {
    DOMException_HierarchyRequestErr("Not allowed to have children");
    return 0;
  }

  if (!(Element_Check(child) || ProcessingInstruction_Check(child) ||
        Comment_Check(child) || Text_Check(child))) {
    if (!Node_Check(child)) {
      /* The child must be a Domlette Node first and foremost. */
      PyErr_BadInternalCall();
    } else {
      PyObject *error =
        PyString_FromFormat("%s nodes cannot be a child of %s nodes",
                            child->ob_type->tp_name, self->ob_type->tp_name);
      if (error != NULL) {
        DOMException_HierarchyRequestErr(PyString_AS_STRING(error));
        Py_DECREF(error);
      }
    }
    return 0;
  }

  /* Everything is OK */
  return 1;
}

/** Public C API ******************************************************/

/* Allocates memory for a new node object of the given type and initializes
 * part of it.
 */
NodeObject *_Node_New(PyTypeObject *type, long flags)
{
  NodeObject *node;

  node = PyObject_GC_New(NodeObject, type);
  if (node != NULL) {
#ifdef DEBUG_NODE_CREATION
    fprintf(stderr, "Created %s node at %p\n", type->tp_name, node);
#endif
    _Node_INIT_FLAGS(node, flags);

    if (flags & Node_FLAGS_CONTAINER) {
      ContainerNode_SET_COUNT(node, 0);
      ContainerNode_SET_ALLOCATED(node, 0);
      ContainerNode_SET_NODES(node, NULL);
    }
  }

  return node;
}

void _Node_Del(NodeObject *node)
{
#ifdef DEBUG_NODE_CREATION
  fprintf(stderr, "Destroyed %s node at %p\n", node->ob_type->tp_name, node);
#endif

  if (node->flags & Node_FLAGS_CONTAINER) {
    NodeObject **nodes = ContainerNode_GET_NODES(node);
    if (nodes) {
      Py_ssize_t i = ContainerNode_GET_COUNT(node);
      while (--i >= 0) {
        Py_DECREF(nodes[i]);
      }
      PyMem_Free(nodes);
    }
  }

  Py_CLEAR(node->parentNode);

  PyObject_GC_Del((PyObject *) node);
}

/* For debugging convenience. */
void _Node_Dump(char *msg, NodeObject *self)
{
  int add_sep;
  fprintf(stderr, "%s\n"
          "  node    : ", msg);
  if (self == NULL) {
    fprintf(stderr, "NULL\n");
  } else {
    PyObject_Print((PyObject *) self, stderr, 0);
    fprintf(stderr, "\n"
            "  flags   :");
    add_sep = 0;
    if (Node_HasFlag(self, Node_FLAGS_CONTAINER)) {
      fprintf(stderr, " Node_FLAGS_CONTAINER");
      add_sep++;
    }
    if (!add_sep) {
      fprintf(stderr, " (none)");
    }
    fprintf(stderr, "\n"
            "  type    : %s\n"
            "  refcount: %" PY_FORMAT_SIZE_T "d\n"
            "  parent  : %p\n",
            self->ob_type == NULL ? "NULL" : self->ob_type->tp_name,
            self->ob_refcnt,
            Node_GET_PARENT(self));
    if (Node_HasFlag(self, Node_FLAGS_CONTAINER)) {
      fprintf(stderr, "  children: %" PY_FORMAT_SIZE_T "d\n",
              ContainerNode_GET_COUNT(self));
    }
  }
  fprintf(stderr, "----------------------\n");
}

/* Semi-private routine for bulk addition of children to a node.
 * This routine is valid for newly contructed container-style nodes which
 * haven't had any children added to them.
 */
int _Node_SetChildren(NodeObject *self, NodeObject **array, Py_ssize_t size)
{
  NodeObject **nodes;
  Py_ssize_t i;

  if (!Node_Check(self) || !Node_HasFlag(self, Node_FLAGS_CONTAINER) ||
      ContainerNode_GET_NODES(self) != NULL) {
    PyErr_BadInternalCall();
    return -1;
  }

  /* Create a copy of the array */
  nodes = PyMem_New(NodeObject *, size);
  if (nodes == NULL) {
    PyErr_NoMemory();
    return -1;
  }
  memcpy(nodes, array, sizeof(NodeObject *) * size);

  /* Set the parent relationship */
  for (i = 0; i < size; i++) {
    assert(Node_GET_PARENT(nodes[i]) == NULL);
    Py_INCREF(self);
    Node_SET_PARENT(nodes[i], self);
  }

  /* Save the new array */
  ContainerNode_SET_NODES(self, nodes);
  ContainerNode_SET_COUNT(self, size);
  ContainerNode_SET_ALLOCATED(self, size);

  return 0;
}

int Node_RemoveChild(NodeObject *self, NodeObject *oldChild)
{
  NodeObject **nodes;
  Py_ssize_t count, index, i;

  if (self == NULL || !Node_Check(self)) {
    PyErr_BadInternalCall();
    return -1;
  } else if (!Node_HasFlag(self, Node_FLAGS_CONTAINER)) {
    DOMException_HierarchyRequestErr("Not allowed to have children");
    return -1;
  }

#ifdef DEBUG_NODE_REMOVE_CHILD
  _Node_Dump("Node_RemoveChild: initial state of self", self);
  _Node_Dump("Node_RemoveChild: initial state of oldChild", oldChild);
#endif

  /* Find the index of the child to be removed */
  nodes = ContainerNode_GET_NODES(self);
  count = ContainerNode_GET_COUNT(self);
  index = -1;
  for (i = ContainerNode_GET_COUNT(self); --i >= 0;) {
    if (nodes[i] == oldChild) {
      index = i;
      break;
    }
  }
  if (index == -1) {
    DOMException_NotFoundErr("Child not found");
    return -1;
  }
  assert(Node_GET_PARENT(oldChild) == self);

#ifdef DEBUG_NODE_REMOVE_CHILD
  fprintf(stderr, "Node_RemoveChild: oldChild found at %d\n",index);
#endif

  /* Set the parent to NULL, indicating no parent */
  Py_DECREF(Node_GET_PARENT(oldChild));
  Node_SET_PARENT(oldChild, NULL);

  /* Now shift the nodes in the array over the top of the removed node */
  memmove(&nodes[index], &nodes[index+1],
          (count - (index + 1)) * sizeof(NodeObject *));
  node_resize((ContainerNodeObject *)self, count - 1);

  /* Drop the reference to the removed node as it is no longer in the array */
  Py_DECREF(oldChild);

#ifdef DEBUG_NODE_REMOVE_CHILD
  _Node_Dump("Node_RemoveChild: final state of self", self);
  _Node_Dump("Node_RemoveChild: final state of oldChild", oldChild);
#endif

  return 0;
}

int Node_AppendChild(NodeObject *self, NodeObject *newChild)
{
  Py_ssize_t count;

  if (!node_validate_child(self, newChild)) {
    return -1;
  }

#ifdef DEBUG_NODE_APPEND_CHILD
  _Node_Dump("Node_AppendChild: initial state of self", self);
  _Node_Dump("Node_AppendChild: initial state of child", newChild);
#endif

  /* Make room for the new child */
  count = ContainerNode_GET_COUNT(self);
  if (node_resize((ContainerNodeObject *)self, count + 1) == -1)
    return -1;

  /* If the child has a previous parent, remove it from that parent */
  if (Node_GET_PARENT(newChild) != NULL) {
    if (Node_RemoveChild(Node_GET_PARENT(newChild), newChild) < 0) {
      /* Forget the new size; it is OK to leave the resize intact */
      ContainerNode_SET_COUNT(self, count);
      return -1;
    }
    assert(Node_GET_PARENT(newChild) == NULL);
  }

  /* Add the new child to the end of our array */
  Py_INCREF(newChild);
  ContainerNode_SET_CHILD(self, count, newChild);

#ifdef DEBUG_NODE_APPEND_CHILD
  _Node_Dump("Node_AppendChild: newChild after append", newChild);
#endif

  /* Set the parent relationship */
  Py_INCREF(self);
  Node_SET_PARENT(newChild, self);

#ifdef DEBUG_NODE_APPEND_CHILD
  _Node_Dump("Node_AppendChild: final state of self", self);
  _Node_Dump("Node_AppendChild: final state of child", newChild);
#endif

  return 0;
}

int Node_InsertBefore(NodeObject *self, NodeObject *newChild,
                      NodeObject *refChild)
{
  NodeObject **nodes;
  Py_ssize_t count, index, i;

  if (!node_validate_child(self, newChild)) {
    return -1;
  }

  if (refChild == (NodeObject *) Py_None) {
#ifdef DEBUG_NODE_INSERT_BEFORE
    fprintf(stderr, "Node_InsertBefore: refChild is None, doing append\n");
#endif
    return Node_AppendChild(self, newChild);
  } else if (!Node_Check(refChild)) {
    PyErr_BadInternalCall();
    return -1;
  }

#ifdef DEBUG_NODE_INSERT_BEFORE
  _Node_Dump("Node_InsertBefore: initial state of self", self);
  _Node_Dump("Node_InsertBefore: initial state of newChild", newChild);
  _Node_Dump("Node_InsertBefore: initial state of refChild", refChild);
#endif

  /* Find the index of the reference node */
  nodes = ContainerNode_GET_NODES(self);
  count = ContainerNode_GET_COUNT(self);
  index = -1;
  for (i = count; --i >= 0;) {
    if (nodes[i] == refChild) {
      index = i;
      break;
    }
  }
  if (index == -1) {
    DOMException_NotFoundErr("refChild not found");
    return -1;
  }

#ifdef DEBUG_NODE_INSERT_BEFORE
  fprintf(stderr, "Node_InsertBefore: refChild found at %d\n", index);
#endif

  /* Make room for the new child */
  if (node_resize((ContainerNodeObject *)self, count + 1) == -1)
    return -1;

  /* If the child has a previous parent, remove it from that parent */
  if (Node_GET_PARENT(newChild) != NULL) {
    if (Node_RemoveChild(Node_GET_PARENT(newChild), newChild) < 0) {
      /* Forget the new size; it is OK to leave the resize intact */
      ContainerNode_SET_COUNT(self, count);
      return -1;
    }
    assert(Node_GET_PARENT(newChild) == NULL);
  }

  /* The pointer to nodes may have changed do to the resize */
  nodes = ContainerNode_GET_NODES(self);
  /* Shift the effected nodes up one */
  for (i = count; --i >= index;)
    nodes[i+1] = nodes[i];

  /* Insert the newChild at the found index in the array */
  Py_INCREF(newChild);
  ContainerNode_SET_CHILD(self, index, newChild);

#ifdef DEBUG_NODE_INSERT_BEFORE
  _Node_Dump("Node_InsertBefore: newChild after insert", newChild);
#endif
  /* Set the parent relationship */
  Py_INCREF(self);
  Node_SET_PARENT(newChild, self);

#ifdef DEBUG_NODE_INSERT_BEFORE
  _Node_Dump("Node_InsertBefore: final state of self", self);
  _Node_Dump("Node_InsertBefore: final state of newChild", newChild);
  _Node_Dump("Node_InsertBefore: final state of refChild", refChild);
#endif

  return 0;
}

int Node_ReplaceChild(NodeObject *self, NodeObject *newChild,
                      NodeObject *oldChild)
{
  NodeObject **nodes;
  Py_ssize_t index;

#ifdef DEBUG_NODE_REPLACE_CHILD
  _Node_Dump("Node_ReplaceChild: initial state of self", self);
  _Node_Dump("Node_ReplaceChild: initial state of newChild", newChild);
  _Node_Dump("Node_ReplaceChild: initial state of oldChild", oldChild);
#endif

  /* Find the index of the reference node */
  nodes = ContainerNode_GET_NODES(self);
  for (index = ContainerNode_GET_COUNT(self); --index >= 0;) {
    if (nodes[index] == oldChild) {
      break;
    }
  }
  if (index == -1) {
    DOMException_NotFoundErr("oldChild not found");
    return -1;
  }
  assert(Node_GET_PARENT(oldChild) == self);

#ifdef DEBUG_NODE_REPLACE_CHILD
  fprintf(stderr, "Node_ReplaceChild: oldChild found at %d\n", index);
#endif

  /* If `newChild` has a previous parent, remove it from that parent */
  if (Node_GET_PARENT(newChild) != NULL) {
    if (Node_RemoveChild(Node_GET_PARENT(newChild), newChild) < 0) {
      return -1;
    }
    assert(Node_GET_PARENT(newChild) == NULL);
  }

  /* Set the parent for `oldChild` to NULL, indicating no parent */
  Py_DECREF(Node_GET_PARENT(oldChild));
  Node_SET_PARENT(oldChild, NULL);

  /* Remove it from the nodes array (just drop the reference to it as its
   * spot will soon be taken by `newChild`) */
  Py_DECREF(oldChild);

#ifdef DEBUG_NODE_REPLACE_CHILD
  _Node_Dump("Node_ReplaceChild: oldChild after remove", oldChild);
#endif

  /* Insert `newChild` at the found index in the array */
  Py_INCREF(newChild);
  ContainerNode_SET_CHILD(self, index, newChild);

  /* Set the parent relationship */
  Py_INCREF(self);
  Node_SET_PARENT(newChild, self);

#ifdef DEBUG_NODE_REPLACE_CHILD
  _Node_Dump("replaceChild: final state of self", self);
  _Node_Dump("replaceChild: final state of newChild", newChild);
  _Node_Dump("replaceChild: final state of oldChild", oldChild);
#endif

  return 0;
}

/** Python Methods *****************************************************/

static char normalize_doc[] = "\
Puts all Text nodes in the full depth of the sub-tree underneath this Node,\n\
including attribute nodes, into a \"normal\" form where only structure\n\
(e.g., elements, comments, processing instructions, CDATA sections, and\n\
entity references) separates Text nodes, i.e., there are neither adjacent\n\
Text nodes nor empty Text nodes.";

static PyObject *node_normalize(NodeObject *self, PyObject *args)
{
  Py_ssize_t i, count;

  if (!PyArg_ParseTuple(args, ":normalize"))
    return NULL;

#ifdef DEBUG_NODE_NORMALIZE
  _Node_Dump("normalize: initial state of self", self);
#endif

  if (!Node_HasFlag(self, Node_FLAGS_CONTAINER)) {
    Py_INCREF(Py_None);
    return Py_None;
  }

  if (ContainerNode_GET_COUNT(self) < 2) {
    Py_INCREF(Py_None);
    return Py_None;
  }

  /* Count to the length minus 1 as the last node has no following sibling
   * with which to be merged. */
  for (i = 0, count = ContainerNode_GET_COUNT(self); i < count; i++) {
    NodeObject *current = ContainerNode_GET_CHILD(self, i);
#ifdef DEBUG_NODE_NORMALIZE
    _Node_Dump("normalize: current node", current);
#endif
    /* If this node is a Text node, determine if following siblings are also
     * Text nodes.
     */
    if (Text_Check(current)) {
      /* Loop over the following siblings */
      for (i++; i < count; count--) {
        NodeObject *next = ContainerNode_GET_CHILD(self, i);
#ifdef DEBUG_NODE_NORMALIZE
        _Node_Dump("normalize: next node", next);
#endif
        if (!Text_Check(next)) break;
        /* Adjacent Text nodes; merge their data. */
        PyObject *current_value, *next_value, *new_value;
        Py_UNICODE *raw_value;
        current_value = Text_GET_DATA(current);
        next_value = Text_GET_DATA(next);
        new_value = PyUnicode_FromUnicode(NULL,
                                          PyUnicode_GET_SIZE(current_value) +
                                          PyUnicode_GET_SIZE(next_value));
        if (new_value == NULL) return NULL;
        raw_value = PyUnicode_AS_UNICODE(new_value);
        Py_UNICODE_COPY(raw_value, PyUnicode_AS_UNICODE(current_value),
                        PyUnicode_GET_SIZE(current_value));
        raw_value += PyUnicode_GET_SIZE(current_value);
        Py_UNICODE_COPY(raw_value, PyUnicode_AS_UNICODE(next_value),
                        PyUnicode_GET_SIZE(next_value));
        Text_SET_DATA(current, new_value);
        Py_DECREF(current_value);

#ifdef DEBUG_NODE_NORMALIZE
        _Node_Dump("normalize: current after merge", current);
#endif
        /* Remove the sibling. */
        if (Node_RemoveChild(self, next) < 0)
          return NULL;
#ifdef DEBUG_NODE_NORMALIZE
        _Node_Dump("normalize: self after merge", self);
#endif
      }
    }
  }

#ifdef DEBUG_NODE_NORMALIZE
  _Node_Dump("normalize: final state of self", self);
#endif

  Py_INCREF(Py_None);
  return Py_None;
}

static char removeChild_doc[] = "\
Removes the child node indicated by oldChild from the list of children, and\n\
returns it.";

static PyObject *node_removeChild(NodeObject *self, PyObject *args)
{
  NodeObject *oldChild;

  if (!PyArg_ParseTuple(args, "O!:xml_remove_child", &DomletteNode_Type, &oldChild))
    return NULL;

  if (Node_RemoveChild(self, oldChild) == -1)
    return NULL;

  Py_INCREF(oldChild);
  return (PyObject *) oldChild;
}

static char appendChild_doc[] = "\
Adds the node newChild to the end of the list of children of this node.";

static PyObject *node_appendChild(NodeObject *self, PyObject *args)
{
  NodeObject *newChild;

  if (!PyArg_ParseTuple(args,"O!:xml_append_child", &DomletteNode_Type, &newChild))
    return NULL;

  if (Node_AppendChild(self, newChild) == -1)
    return NULL;

  Py_INCREF(newChild);
  return (PyObject *) newChild;
}

static char insertBefore_doc[] = "\
Inserts the node newChild before the existing child node refChild.";

static PyObject *node_insertBefore(NodeObject *self, PyObject *args)
{
  NodeObject *newChild;
  PyObject *refChild;

  if (!PyArg_ParseTuple(args, "O!O:xml_insert_before",
                        &DomletteNode_Type, &newChild, &refChild))
    return NULL;

  if (refChild != Py_None && !Node_Check(refChild)) {
    PyErr_SetString(PyExc_TypeError, "arg 2 must be Node or None");
    return NULL;
  }

  if (Node_InsertBefore(self, newChild, (NodeObject *)refChild) == -1)
    return NULL;

  Py_INCREF(newChild);
  return (PyObject *) newChild;
}

static char replaceChild_doc[] = "\
Replaces the child node oldChild with newChild in the list of children, and\n\
returns the oldChild node.";

static PyObject *node_replaceChild(NodeObject *self, PyObject *args)
{
  NodeObject *newChild, *oldChild;

  if (!PyArg_ParseTuple(args, "O!O!:xml_replace_child",
                        &DomletteNode_Type, &newChild,
                        &DomletteNode_Type, &oldChild))
    return NULL;

  if (Node_ReplaceChild(self, newChild, oldChild) < 0)
    return NULL;

  Py_INCREF(oldChild);
  return (PyObject *) oldChild;
}

static char select_doc[] = "\
Evaluates an XPath expression string using this node as context.";

static PyObject *node_select(NodeObject *self, PyObject *args, PyObject *kw)
{
  PyObject *expr, *explicit_nss = Py_None;
  PyObject *module, *result;
  static char *kwlist[] = { "expr", "prefixes", NULL };

  if (!PyArg_ParseTupleAndKeywords(args, kw, "O|O:xml_select", kwlist,
                                   &expr, &explicit_nss))
    return NULL;

  module = PyImport_ImportModule("amara.xpath.util");
  if (module == NULL) return NULL;
  result = PyObject_CallMethod(module, "SimpleEvaluate", "OOO",
                               expr, self, explicit_nss);
  Py_DECREF(module);
  return result;
}

static PyObject *node_copy(PyObject *self, PyObject *noargs)
{
  PyObject *info, *callable, *args, *state=NULL;
  PyObject *result;

  info = PyObject_CallMethod(self, "__reduce__", NULL);
  if (info == NULL)
    return NULL;
  if (!PyArg_UnpackTuple(info, NULL, 2, 3, &callable, &args, &state)) {
    Py_DECREF(info);
    return NULL;
  }
  Py_DECREF(info);

  result = PyObject_CallObject(callable, args);
  if (result == NULL)
    return NULL;
  if (state != NULL) {
    info = PyObject_CallMethod(result, "__setstate__", "O", state);
    if (info == NULL) {
      Py_DECREF(result);
      return NULL;
    }
    Py_DECREF(info);
  }
  return result;
}

static PyObject *node_deepcopy(PyObject *self, PyObject *memo)
{
  PyObject *info, *callable, *args, *state=NULL;
  PyObject *module, *deepcopy, *result;

  info = PyObject_CallMethod(self, "__reduce__", NULL);
  if (info == NULL)
    return NULL;
  if (!PyArg_UnpackTuple(info, NULL, 2, 3, &callable, &args, &state)) {
    Py_DECREF(info);
    return NULL;
  }
  Py_DECREF(info);

  module = PyImport_ImportModule("copy");
  if (module == NULL)
    return NULL;
  deepcopy = PyObject_GetAttrString(module, "deepcopy");
  Py_DECREF(module);
  if (deepcopy == NULL)
    return NULL;

  args = PyObject_CallFunctionObjArgs(deepcopy, args, memo, NULL);
  if (args == NULL) {
    Py_DECREF(deepcopy);
    return NULL;
  }
  result = PyObject_CallObject(callable, args);
  Py_DECREF(args);
  if (result == NULL) {
    Py_DECREF(deepcopy);
    return NULL;
  }
  if (PyDict_SetItem(memo, self, result) < 0) {
    Py_DECREF(result);
    Py_DECREF(deepcopy);
    return NULL;
  }
  if (state != NULL) {
    state = PyObject_CallFunctionObjArgs(deepcopy, state, memo, NULL);
    if (state == NULL) {
      Py_DECREF(result);
      Py_DECREF(deepcopy);
      return NULL;
    }
    info = PyObject_CallMethod(result, "__setstate__", "O", state);
    if (info == NULL) {
      Py_DECREF(result);
      Py_DECREF(deepcopy);
      return NULL;
    }
    Py_DECREF(info);
  }
  Py_DECREF(deepcopy);
  return result;
}

static PyObject *node_reduce(PyObject *self, PyObject *noargs)
{
  PyObject *newargs, *args, *arg, *state, *info;
  Py_ssize_t i, n;

  newargs = PyObject_CallMethod(self, "__getnewargs__", NULL);
  if (newargs == NULL)
    return NULL;
  if (!PyTuple_Check(newargs)) {
    PyErr_Format(PyExc_TypeError,
                  "__getnewargs__() should return a tuple, not '%.200s'",
                  newargs == Py_None ? "None" : newargs->ob_type->tp_name);
    Py_DECREF(newargs);
    return NULL;
  }
  /* construct the argumnt list of (cls, *newargs) */
  n = PyTuple_GET_SIZE(newargs);
  args = PyTuple_New(n + 1);
  if (args == NULL) {
    Py_DECREF(newargs);
    return NULL;
  }
  i = 0;
  arg = (PyObject *)self->ob_type;
  do {
    PyTuple_SET_ITEM(args, i, arg);
    Py_INCREF(arg);
    if (i >= n)
      break;
    arg = PyTuple_GET_ITEM(newargs, i++);
  } while (1);
  Py_DECREF(newargs);

  state = PyObject_CallMethod(self, "__getstate__", NULL);
  if (state == NULL) {
    Py_DECREF(args);
    return NULL;
  }

  info = PyTuple_New(3);
  if (info == NULL) {
    Py_DECREF(args);
    Py_DECREF(state);
  } else {
    PyTuple_SET_ITEM(info, 0, newobj_function);
    Py_INCREF(newobj_function);
    PyTuple_SET_ITEM(info, 1, args);
    PyTuple_SET_ITEM(info, 2, state);
  }
  return info;
}

static PyObject *node_getnewargs(PyObject *self, PyObject *noarg)
{
  return PyTuple_New(0);
}

static PyObject *node_getstate(PyObject *self, PyObject *args)
{
  PyObject *deep=Py_True, *state;

  if (!PyArg_ParseTuple(args, "|O:__getstate__", &deep))
    return NULL;

  state = (PyObject *)Node_GET_PARENT(self);
  Py_INCREF(state);
  return state;
}

static PyObject *node_setstate(PyObject *self, PyObject *state)
{
  NodeObject *parent, *temp;

  if (Node_Check(state))
    parent = Node(state);
  else
    return PyErr_Format(PyExc_NotImplementedError,
                        "subclass '%s' must override __setstate__()",
                        self->ob_type->tp_name);

  temp = Node_GET_PARENT(self);
  Node_SET_PARENT(self, parent);
  Py_INCREF(parent);
  Py_XDECREF(temp);

  Py_INCREF(Py_None);
  return Py_None;
}

#define Node_METHOD(NAME, ARGSPEC) \
  { "xml_" #NAME, (PyCFunction) node_##NAME, ARGSPEC, NAME##_doc }

static PyMethodDef node_methods[] = {
  Node_METHOD(normalize,     METH_VARARGS),
  //Node_METHOD(removeChild,   METH_VARARGS),
  //Node_METHOD(appendChild,   METH_VARARGS),
  //Node_METHOD(insertBefore,  METH_VARARGS),
  //Node_METHOD(replaceChild,  METH_VARARGS),
  Node_METHOD(select,         METH_KEYWORDS),
  { "__copy__",       node_copy,       METH_NOARGS,  "helper for copy" },
  { "__deepcopy__",   node_deepcopy,   METH_O,       "helper for deepcopy" },
  { "__reduce__",     node_reduce,     METH_NOARGS,  "helper for pickle" },
  { "__getnewargs__", node_getnewargs, METH_NOARGS,  "helper for pickle" },
  { "__getstate__",   node_getstate,   METH_VARARGS, "helper for pickle" },
  { "__setstate__",   node_setstate,   METH_O,       "helper for pickle" },
  { NULL }
};

/** Python Members ****************************************************/

#define Node_MEMBER(NAME) \
  { #NAME, T_OBJECT, offsetof(NodeObject, NAME), RO }

static PyMemberDef node_members[] = {
  Node_MEMBER(parentNode),
  { NULL }
};

static PyObject *get_child_nodes(NodeObject *self, void *arg)
{
  register Py_ssize_t i;
  register Py_ssize_t len;

  PyObject *childNodes;

  if (Node_HasFlag(self, Node_FLAGS_CONTAINER)) {
    len = ContainerNode_GET_COUNT(self);
    childNodes = PyList_New(len);
    if (childNodes == NULL) return NULL;
    for (i = 0; i < len; i++) {
      PyObject *child = (PyObject *) ContainerNode_GET_CHILD(self, i);
      Py_INCREF(child);
      PyList_SET_ITEM(childNodes, i, child);
    }
  } else {
    childNodes = PyList_New(0);
  }
  return childNodes;
}

static PyObject *get_base_uri(NodeObject *self, void *arg)
{
  NodeObject *node = self;
  PyObject *base, *result;

  /* DOM3 baseURI is calculated according to XML Base */

  while (Node_GET_PARENT(node) != NULL) {
    /* 1. the base URI specified by an xml:base attribute on the element,
     *    if one exists, otherwise
     */
    if (Element_Check(node)) {
      base = PyDict_GetItem(Element_GET_ATTRIBUTES(node), xml_base_key);
      if (base) {
        base = Attr_GET_NODE_VALUE(base);
        /* If the xml:base in scope for the current node is not absolute, we find
         * the element where that xml:base was declared, then Absolutize our
         * relative xml:base against the base URI of the parent of declaring
         * element, recursively. */
        result = PyObject_CallFunction(is_absolute_function, "O", base);
        if (result == NULL) return NULL;
        switch (PyObject_IsTrue(result)) {
        case 0:
          Py_DECREF(result);
          result = get_base_uri(Node_GET_PARENT(node), arg);
          if (result == NULL) return NULL;
          else if (result == Py_None) return result;
          base = PyObject_CallFunction(absolutize_function, "OO", base, result);
          if (base == NULL) {
            Py_DECREF(result);
            return NULL;
          }
          /* fall through */
        case 1:
          Py_DECREF(result);
          Py_INCREF(base);
          return base;
        default:
          return NULL;
        }
      }
    }

    /* 2. the base URI of the element's parent element within the document
     *    or external entity, if one exists, otherwise
     */
    node = Node_GET_PARENT(node);
  }

  /* 3. the base URI of the document entity or external entity containing the
   *    element.
   */
  if (Document_Check(node)) {
    base = Document_GET_DOCUMENT_URI(node);
    result = PyObject_CallFunction(is_absolute_function, "O", base);
    if (result == NULL) return NULL;
    switch (PyObject_IsTrue(result)) {
    case 0:
      base = Py_None;
      /* fall through */
    case 1:
      break;
    default:
      return NULL;
    }
  } else {
    /* Node does not yet have a parent */
    base = Py_None;
  }

  Py_INCREF(base);
  return base;
}

static PyObject *get_first_child(NodeObject *self, void *arg)
{
  PyObject *child;

  if (Node_HasFlag(self, Node_FLAGS_CONTAINER) &&
      ContainerNode_GET_COUNT(self)) {
    child = (PyObject *) ContainerNode_GET_CHILD(self, 0);
  } else {
    child = Py_None;
  }
  Py_INCREF(child);
  return child;
}

static PyObject *get_last_child(NodeObject *self, void *arg)
{
  PyObject *child;

  if (Node_HasFlag(self, Node_FLAGS_CONTAINER) &&
      ContainerNode_GET_COUNT(self)) {
    child = (PyObject *)
      ContainerNode_GET_CHILD(self, ContainerNode_GET_COUNT(self) - 1);
  } else {
    child = Py_None;
  }
  Py_INCREF(child);
  return child;
}

static PyObject *get_next_sibling(NodeObject *self, void *arg)
{
  NodeObject *parentNode;
  NodeObject **nodes;
  PyObject *sibling;
  Py_ssize_t count, index;

  parentNode = self->parentNode;
  if (parentNode == NULL) {
    Py_INCREF(Py_None);
    return Py_None;
  }

  nodes = ContainerNode_GET_NODES(parentNode);
  count = ContainerNode_GET_COUNT(parentNode);
  for (index = 0; index < count; index++) {
    if (nodes[index] == self) {
      /* advance to the following node */
      index++;
      if (index == count) /* last child */
        sibling = Py_None;
      else
        sibling = (PyObject *) nodes[index];
      Py_INCREF(sibling);
      return sibling;
    }
  }

  return DOMException_InvalidStateErr("lost from parent");
}

static PyObject *get_previous_sibling(NodeObject *self, void *arg)
{
  NodeObject *parentNode;
  NodeObject **nodes;
  PyObject *sibling;
  Py_ssize_t count, index;

  parentNode = self->parentNode;
  if (parentNode == NULL) {
    Py_INCREF(Py_None);
    return Py_None;
  }

  nodes = ContainerNode_GET_NODES(parentNode);
  count = ContainerNode_GET_COUNT(parentNode);
  for (index = 0; index < count; index++) {
    if (nodes[index] == self) {
      if (index == 0) /* first child */
        sibling = Py_None;
      else
        sibling = (PyObject *) nodes[index - 1];
      Py_INCREF(sibling);
      return sibling;
    }
  }

  return DOMException_InvalidStateErr("lost from parent");
}

/* This is defined as a function to prevent corruption of shared "NodeList" */
static PyObject *get_empty_list(NodeObject *self, void *arg)
{
  return PyList_New(0);
}

static PyGetSetDef node_getset[] = {
  { "xml_children",      (getter)get_child_nodes },
  { "xml_base",         (getter)get_base_uri },
  { "xml_first_child",      (getter)get_first_child },
  { "xml_last_child",       (getter)get_last_child },
  { "xml_next_sibling",     (getter)get_next_sibling },
  { "xml_previous_sibling", (getter)get_previous_sibling },
  { "xml_attributes",  (getter)get_empty_list },
  { "xml_namespaces",  (getter)get_empty_list },
  { NULL }
};

/** Type Object ********************************************************/

static PyObject *node_repr(NodeObject *self)
{
  return PyString_FromFormat("<%s at %p>", self->ob_type->tp_name, self);
}

static int node_traverse(NodeObject *self, visitproc visit, void *arg)
{
#ifdef DEBUG_NODE_CREATION
  printf("Traversing %s node at %p\n", self->ob_type->tp_name, self);
#endif
  Py_VISIT((PyObject *)Node_GET_PARENT(self));
  if (self->flags & Node_FLAGS_CONTAINER) {
    NodeObject **nodes = ContainerNode_GET_NODES(self);
    Py_ssize_t i = ContainerNode_GET_COUNT(self);
    while (--i >= 0) {
      int rt = visit((PyObject *)nodes[i], arg);
      if (rt) return rt;
    }
  }
  return 0;
}

static int node_clear(NodeObject *self)
{
#ifdef DEBUG_NODE_CREATION
  printf("Clearing %s node at %p\n", self->ob_type->tp_name, self);
#endif
  Py_CLEAR(Node_GET_PARENT(self));
  if (Node_HasFlag(self, Node_FLAGS_CONTAINER)) {
    NodeObject **nodes = ContainerNode_GET_NODES(self);
    Py_ssize_t i = ContainerNode_GET_COUNT(self);
    if (nodes != NULL) {
      ContainerNode_SET_NODES(self, NULL);
      ContainerNode_SET_COUNT(self, 0);
      ContainerNode_SET_ALLOCATED(self, 0);
      while (--i >= 0) {
        Py_DECREF(nodes[i]);
      }
      PyMem_Free(nodes);
    }
  }
  return 0;
}

static long node_hash(NodeObject *self)
{
#if SIZEOF_LONG >= SIZEOF_VOID_P
  return (long)self;
#else
  /* convert to a Python long and hash that */
  PyObject *longobj;
  long hash;

  if ((longobj = PyLong_FromVoidPtr(self)) == NULL) {
    return -1;
  }

  hash = PyObject_Hash(longobj);
  Py_DECREF(longobj);
  return hash;
#endif
}

#define OPSTR(op) (op == Py_LT ? "Py_LT" : \
                   (op == Py_LE ? "Py_LE" : \
                    (op == Py_EQ ? "Py_EQ" : \
                     (op == Py_NE ? "Py_NE" : \
                      (op == Py_GE ? "Py_GE" : \
                       (op == Py_GT ? "Py_GT" : "?"))))))

#define BOOLSTR(ob) (ob == Py_True ? "Py_True" : \
                     (ob == Py_False ? "Py_False" : \
                      (ob == Py_NotImplemented ? "Py_NotImplemented" : \
                       (ob == NULL ? "NULL" : "?"))))

#define NODESTR(node) PyString_AS_STRING(PyObject_Repr(node))

static PyObject *node_richcompare(NodeObject *a, NodeObject *b, int op)
{
  PyObject *doc_a, *doc_b, *result;
  NodeObject *parent_a, *parent_b;
  int depth_a, depth_b;

  /* Make sure both arguments are cDomlette nodes */
  if (!(Node_Check(a) && Node_Check(b))) {
    Py_INCREF(Py_NotImplemented);
    return Py_NotImplemented;
  }

  if (a == b) {
    /* same objects */
    switch (op) {
    case Py_EQ: case Py_LE: case Py_GE:
      result = Py_True;
      break;
    case Py_NE: case Py_LT: case Py_GT:
      result = Py_False;
      break;
    default:
      result = Py_NotImplemented;
    }

    Py_INCREF(result);
    return result;
  }

  /* traverse to the top of each tree (document, element or the node itself)
  */
  parent_a = a;
  depth_a = 0;
  while (Node_GET_PARENT(parent_a)) {
    parent_a = Node_GET_PARENT(parent_a);
    depth_a++;
  }

  parent_b = b;
  depth_b = 0;
  while (Node_GET_PARENT(parent_b)) {
    parent_b = Node_GET_PARENT(parent_b);
    depth_b++;
  }

  /* compare the top of each tree; for Documents use the creation index,
   * otherwise None for trees not rooted in a Document. If both trees do
   * not have a Document root, fall back to default Python comparison. */
  doc_a = Document_Check(parent_a) ? Document_GET_INDEX(parent_a) : Py_None;
  doc_b = Document_Check(parent_b) ? Document_GET_INDEX(parent_b) : Py_None;
  if (doc_a != doc_b) {
    return PyObject_RichCompare(doc_a, doc_b, op);
  }
  else if (parent_a != parent_b) {
    Py_INCREF(Py_NotImplemented);
    return Py_NotImplemented;
  }

  /* if neither node is a document (depth>0), find the nodes common ancestor */
  if (depth_a > 0 && depth_b > 0) {
    NodeObject **nodes;
    Py_ssize_t i, count;

    /* traverse to the same depth in the tree for both nodes */
    for (i = depth_a; i > depth_b; i--) {
      a = Node_GET_PARENT(a);
    }

    for (i = depth_b; i > depth_a; i--) {
      b = Node_GET_PARENT(b);
    }

    /* find the nodes common parent */
    if (a != b) {
      parent_a = Node_GET_PARENT(a);
      parent_b = Node_GET_PARENT(b);
      while (parent_a != parent_b) {
        a = parent_a;
        parent_a = Node_GET_PARENT(parent_a);
        b = parent_b;
        parent_b = Node_GET_PARENT(parent_b);
      }

      /* get the nodes position in the child list */
      depth_a = depth_b = -1;
      nodes = ContainerNode_GET_NODES(parent_a);
      count = ContainerNode_GET_COUNT(parent_a);
      for (i = 0; i < count; i++) {
        if (nodes[i] == a)
          depth_a = i;
        else if (nodes[i] == b)
          depth_b = i;
      }
    }
  }

  switch (op) {
  case Py_LT:
    result = (depth_a < depth_b) ? Py_True : Py_False;
    break;
  case Py_LE:
    result = (depth_a <= depth_b) ? Py_True : Py_False;
    break;
  case Py_EQ:
    result = (depth_a == depth_b) ? Py_True : Py_False;
    break;
  case Py_NE:
    result = (depth_a != depth_b) ? Py_True : Py_False;
    break;
  case Py_GT:
    result = (depth_a > depth_b) ? Py_True : Py_False;
    break;
  case Py_GE:
      result = (depth_a >= depth_b) ? Py_True : Py_False;
    break;
  default:
    result = Py_NotImplemented;
  }

  /*
    PySys_WriteStdout("op: %s, a <%p>: %d, b <%p>: %d, result: %s\n",
                      OPSTR(op), a, depth_a, b, depth_b, BOOLSTR(result));
  */

  Py_INCREF(result);
  return result;
}

static PyObject *node_iter(NodeObject *node);

static char node_doc[] = "\
The Node type is the primary datatype for the entire Document Object Model.";

PyTypeObject DomletteNode_Type = {
  /* PyObject_HEAD     */ PyObject_HEAD_INIT(NULL)
  /* ob_size           */ 0,
  /* tp_name           */ Domlette_MODULE_NAME "." "Node",
  /* tp_basicsize      */ sizeof(NodeObject),
  /* tp_itemsize       */ 0,
  /* tp_dealloc        */ (destructor) _Node_Del,
  /* tp_print          */ (printfunc) 0,
  /* tp_getattr        */ (getattrfunc) 0,
  /* tp_setattr        */ (setattrfunc) 0,
  /* tp_compare        */ (cmpfunc) 0,
  /* tp_repr           */ (reprfunc) node_repr,
  /* tp_as_number      */ (PyNumberMethods *) 0,
  /* tp_as_sequence    */ (PySequenceMethods *) 0,
  /* tp_as_mapping     */ (PyMappingMethods *) 0,
  /* tp_hash           */ (hashfunc) node_hash,
  /* tp_call           */ (ternaryfunc) 0,
  /* tp_str            */ (reprfunc) 0,
  /* tp_getattro       */ (getattrofunc) 0,
  /* tp_setattro       */ (setattrofunc) 0,
  /* tp_as_buffer      */ (PyBufferProcs *) 0,
  /* tp_flags          */ (Py_TPFLAGS_DEFAULT |
                           Py_TPFLAGS_BASETYPE |
                           Py_TPFLAGS_HAVE_GC),
  /* tp_doc            */ (char *) node_doc,
  /* tp_traverse       */ (traverseproc) node_traverse,
  /* tp_clear          */ (inquiry) node_clear,
  /* tp_richcompare    */ (richcmpfunc) node_richcompare,
  /* tp_weaklistoffset */ 0,
  /* tp_iter           */ (getiterfunc) node_iter,
  /* tp_iternext       */ (iternextfunc) 0,
  /* tp_methods        */ (PyMethodDef *) node_methods,
  /* tp_members        */ (PyMemberDef *) node_members,
  /* tp_getset         */ (PyGetSetDef *) node_getset,
  /* tp_base           */ (PyTypeObject *) 0,
  /* tp_dict           */ (PyObject *) 0,
  /* tp_descr_get      */ (descrgetfunc) 0,
  /* tp_descr_set      */ (descrsetfunc) 0,
  /* tp_dictoffset     */ 0,
  /* tp_init           */ (initproc) 0,
  /* tp_alloc          */ (allocfunc) 0,
  /* tp_new            */ (newfunc) 0,
  /* tp_free           */ 0,
};


/** Node ChildNodes Iterator ******************************************/

typedef struct {
  PyObject_HEAD
  Py_ssize_t index;
  NodeObject *node; /* NULL when iterator is done */
} NodeIterObject;

static PyTypeObject NodeIter_Type;

static PyObject *node_iter(NodeObject *node)
{
  NodeIterObject *iter;

  iter = PyObject_GC_New(NodeIterObject, &NodeIter_Type);
  if (iter == NULL)
    return NULL;

  iter->index = 0;

  if (Node_HasFlag(node, Node_FLAGS_CONTAINER))
    Py_INCREF(node);
  else
    node = NULL; /* mark iterator as done */
  iter->node = node;

  PyObject_GC_Track(iter);

  return (PyObject *) iter;
}

static void nodeiter_dealloc(NodeIterObject *iter)
{
  PyObject_GC_UnTrack(iter);

  Py_XDECREF(iter->node);

  PyObject_GC_Del(iter);
}

static int nodeiter_traverse(NodeIterObject *iter, visitproc visit, void *arg)
{
  if (iter->node == NULL)
    return 0;
  return visit((PyObject *)iter->node, arg);
}

static PyObject *nodeiter_iter(NodeIterObject *iter)
{
  Py_INCREF(iter);
  return (PyObject *)iter;
}

static PyObject *nodeiter_next(NodeIterObject *iter)
{
  NodeObject *node = iter->node;
  if (node == NULL)
    return NULL;

  if (iter->index < ContainerNode_GET_COUNT(node)) {
    PyObject *item = (PyObject *) ContainerNode_GET_CHILD(node, iter->index++);
    Py_INCREF(item);
    return item;
  }

  Py_DECREF(node);
  iter->node = NULL;
  return NULL;
}

static PyTypeObject NodeIter_Type = {
  /* PyObject_HEAD     */ PyObject_HEAD_INIT(NULL)
  /* ob_size           */ 0,
  /* tp_name           */ "nodeiter",
  /* tp_basicsize      */ sizeof(NodeIterObject),
  /* tp_itemsize       */ 0,
  /* tp_dealloc        */ (destructor) nodeiter_dealloc,
  /* tp_print          */ (printfunc) 0,
  /* tp_getattr        */ (getattrfunc) 0,
  /* tp_setattr        */ (setattrfunc) 0,
  /* tp_compare        */ (cmpfunc) 0,
  /* tp_repr           */ (reprfunc) 0,
  /* tp_as_number      */ (PyNumberMethods *) 0,
  /* tp_as_sequence    */ (PySequenceMethods *) 0,
  /* tp_as_mapping     */ (PyMappingMethods *) 0,
  /* tp_hash           */ (hashfunc) 0,
  /* tp_call           */ (ternaryfunc) 0,
  /* tp_str            */ (reprfunc) 0,
  /* tp_getattro       */ (getattrofunc) 0,
  /* tp_setattro       */ (setattrofunc) 0,
  /* tp_as_buffer      */ (PyBufferProcs *) 0,
  /* tp_flags          */ (Py_TPFLAGS_DEFAULT |
                           Py_TPFLAGS_HAVE_GC),
  /* tp_doc            */ (char *) 0,
  /* tp_traverse       */ (traverseproc) nodeiter_traverse,
  /* tp_clear          */ (inquiry) 0,
  /* tp_richcompare    */ (richcmpfunc) 0,
  /* tp_weaklistoffset */ 0,
  /* tp_iter           */ (getiterfunc) nodeiter_iter,
  /* tp_iternext       */ (iternextfunc) nodeiter_next,
};

/** Module Interface **************************************************/

static PyObject *newobj_call(PyObject *module, PyObject *args)
{
  PyObject *cls, *newargs, *obj;
  PyTypeObject *type;
  Py_ssize_t len;

  assert(PyTuple_Check(args));
  len = PyTuple_GET_SIZE(args);
  if (len < 1) {
    PyErr_SetString(PyExc_TypeError, 
                    "__newobj__() takes at least 1 argument (0 given)");
    return NULL;
  }

  cls = PyTuple_GET_ITEM(args, 0);
  if (!PyType_Check(cls)) {
    PyErr_Format(PyExc_TypeError,
                 "__newobj__() argument 1 must be type, not %.50s",
                 cls == Py_None ? "None" : cls->ob_type->tp_name);
    return NULL;
  }

  type = (PyTypeObject *)cls;
  if (type->tp_new == NULL) {
    PyErr_Format(PyExc_TypeError, "type '%.100s' has NULL tp_new slot",
                 type->tp_name);
    return NULL;
  }

  /* create the argument tuple for the __new__ method */
  newargs = PyTuple_New(len - 1);
  if (newargs == NULL)
    return NULL;
  while (len > 1) {
    PyObject *item = PyTuple_GET_ITEM(args, len--);
    Py_INCREF(item);
    PyTuple_SET_ITEM(newargs, len, item);
  }

  /* call __new__ */
  obj = type->tp_new(type, newargs, NULL);
  Py_DECREF(newargs);
  return obj;
}

static PyMethodDef newobj_meth = {
  /* ml_name  */ "__newobj__",
  /* ml_meth  */ newobj_call,
  /* ml_flags */ METH_VARARGS,
  /* ml_doc   */ "helper for pickle",
};

int DomletteNode_Init(PyObject *module)
{
  PyObject *import, *dict, *value;

  dict = PyModule_GetDict(module);
  newobj_function = PyCFunction_NewEx(&newobj_meth, NULL,
                                      PyDict_GetItemString(dict, "__name__"));
  if (newobj_function == NULL)
    return -1;
  if (PyDict_SetItemString(dict, newobj_meth.ml_name, newobj_function) < 0) {
    Py_DECREF(newobj_function);
    return -1;
  }

  import = PyImport_ImportModule("amara.lib.iri");
  if (import == NULL) return -1;
  is_absolute_function = PyObject_GetAttrString(import, "is_absolute");
  if (is_absolute_function == NULL) {
    Py_DECREF(import);
    return -1;
  }
  absolutize_function = PyObject_GetAttrString(import, "absolutize");
  if (absolutize_function == NULL) {
    Py_DECREF(import);
    return -1;
  }
  Py_DECREF(import);

  /* Initialize type objects */
  if (PyType_Ready(&DomletteNode_Type) < 0)
    return -1;

  /* Grrrr...MingW32 gcc doesn't support assigning imported functions in a
   * static structure.  This sucks because both gcc/Unix and MSVC both support
   * that.
   */
  NodeIter_Type.tp_getattro = PyObject_GenericGetAttr;
  if (PyType_Ready(&NodeIter_Type) < 0)
    return -1;

  /* Assign "class" constants */
  dict = DomletteNode_Type.tp_dict;
  value = PyString_FromString("node");
  if (value == NULL)
    return -1;
  if (PyDict_SetItemString(dict, "xml_type", value) < 0)
    return -1;
  Py_DECREF(value);

  shared_empty_nodelist = PyList_New(0);
  if (shared_empty_nodelist == NULL)
    return -1;

  xml_base_key = Py_BuildValue("(ss)", 
                               "http://www.w3.org/XML/1998/namespace", "base");
  if (xml_base_key == NULL)
    return -1;

  Py_INCREF(&DomletteNode_Type);
  return PyModule_AddObject(module, "Node", (PyObject*)&DomletteNode_Type);
}

void DomletteNode_Fini(void)
{
  Py_DECREF(newobj_function);
  Py_DECREF(shared_empty_nodelist);
  Py_DECREF(xml_base_key);

  PyType_CLEAR(&DomletteNode_Type);
  PyType_CLEAR(&NodeIter_Type);
}
