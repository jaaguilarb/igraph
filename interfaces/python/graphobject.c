/* -*- mode: C -*-  */
/* 
   IGraph library.
   Copyright (C) 2006  Gabor Csardi <csardi@rmki.kfki.hu>
   MTA RMKI, Konkoly-Thege Miklos st. 29-33, Budapest 1121, Hungary
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc.,  51 Franklin Street, Fifth Floor, Boston, MA 
   02110-1301 USA

*/

#include "common.h"
#include "graphobject.h"
#include "vertexseqobject.h"
#include "edgeseqobject.h"
#include "bfsiter.h"
#include "convert.h"
#include "error.h"

#define ATTRHASH_IDX_GRAPH 0
#define ATTRHASH_IDX_VERTEX 1
#define ATTRHASH_IDX_EDGE 2

PyTypeObject igraphmodule_GraphType;

/** \defgroup python_interface_graph Graph object
 * \ingroup python_interface */

/**
 * \ingroup python_interface_internal
 * \brief Initializes the internal structures in an \c igraph.Graph object's
 * C representation.
 * 
 * This function must be called whenever we create a new Graph object with
 * \c tp_alloc
 */
void igraphmodule_Graph_init_internal(igraphmodule_GraphObject *self) {
  if (!self) return;
  self->vseq = NULL;
  self->eseq = NULL;
  self->destructor = NULL;
  self->weakreflist = NULL;
  self->g.attr = NULL;
}

/**
 * \ingroup python_interface_graph
 * \brief Creates a new igraph object in Python
 * 
 * This function is called whenever a new \c igraph.Graph object is created in
 * Python. An optional \c n parameter can be passed from Python,
 * representing the number of vertices in the graph. If it is omitted,
 * the default value is 1.
 * 
 * <b>Example call from Python:</b>
\verbatim
g = igraph.Graph(5);
\endverbatim
 *
 * In fact, the parameters are processed by \c igraphmodule_Graph_init
 * 
 * \return the new \c igraph.Graph object or NULL if an error occurred.
 * 
 * \sa igraphmodule_Graph_init
 * \sa igraph_empty
 */
PyObject* igraphmodule_Graph_new(PyTypeObject *type, PyObject *args,
				 PyObject *kwds) {
  igraphmodule_GraphObject *self;

  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  /* don't need it, the constructor will do it */
  /*if (self != NULL) {
    igraph_empty(&self->g, 1, 0);
  }*/
  igraphmodule_Graph_init_internal(self);
  
  return (PyObject*)self;
}

/**
 * \ingroup python_interface_graph
 * \brief Clears the graph object's subobject (before deallocation)
 */
int igraphmodule_Graph_clear(igraphmodule_GraphObject *self) {
  PyObject *tmp;
  PyObject_GC_UnTrack(self);
  
  tmp=self->vseq;
  self->vseq=NULL;
  Py_XDECREF(tmp);

  tmp=self->eseq;
  self->eseq=NULL;
  Py_XDECREF(tmp);

  tmp=self->destructor;
  self->destructor=NULL;
  Py_XDECREF(tmp);

  /*
  for (i=0; i<3; i++) {
    tmp=self->attrs[i];
    self->attrs[i]=NULL;
    Py_XDECREF(tmp);
  }
  */
  return 0;
}

/**
 * \ingroup python_interface_graph
 * \brief Support for cyclic garbage collection in Python
 * 
 * This is necessary because the \c igraph.Graph object contains several
 * other \c PyObject pointers and they might point back to itself.
 */
int igraphmodule_Graph_traverse(igraphmodule_GraphObject *self,
				visitproc visit, void *arg) {
  int vret, i;

  RC_TRAVERSE("Graph", self);
  
  if (self->destructor) {
    vret=visit(self->destructor, arg);
    if (vret != 0) return vret;
  }
  
  if (self->g.attr) {
    for (i=0; i<3; i++) {
      vret=visit(((PyObject**)(self->g.attr))[i], arg);
      if (vret != 0) return vret;
    }
  }
  
  // Funny things happen when we traverse the contained VertexSeq or EdgeSeq
  // object (it results in obviously fake memory leaks)
  /*if (self->vseq) {
    vret=visit(self->vseq, arg);
    if (vret != 0) return vret;
  }*/
  
  return 0;
}

/**
 * \ingroup python_interface_graph
 * \brief Deallocates a Python representation of a given igraph object
 */
void igraphmodule_Graph_dealloc(igraphmodule_GraphObject* self) 
{
  PyObject* r;

  // Clear weak references
  if (self->weakreflist != NULL)
    PyObject_ClearWeakRefs((PyObject*)self);
  
  igraph_destroy(&self->g);
  
  if (PyCallable_Check(self->destructor)) {
    r=PyObject_CallObject(self->destructor, NULL);
    if (r) {
      Py_DECREF(r);
    }
  }

  igraphmodule_Graph_clear(self);

  RC_DEALLOC("Graph", self);
  
  PyObject_GC_Del((PyObject*)self);
  // self->ob_type->tp_free((PyObject*)self);
}

/**
 * \ingroup python_interface_graph
 * \brief Initializes a new \c igraph object in Python
 * 
 * This function is called whenever a new \c igraph.Graph object is initialized in
 * Python (note that initializing is not equal to creating: an object might
 * be created but not initialized when it is being recovered from a serialized
 * state).
 * 
 * Throws \c AssertionError in Python if \c vcount is less than or equal to zero.
 * \return the new \c igraph.Graph object or NULL if an error occurred.
 * 
 * \sa igraphmodule_Graph_new
 * \sa igraph_empty
 * \sa igraph_create
 */
int igraphmodule_Graph_init(igraphmodule_GraphObject *self,
			    PyObject *args, PyObject *kwds) {
  char *kwlist[] = {"n", "edges", "directed", NULL};
  int n=1;
  PyObject *edges=NULL, *dir=Py_False;
  igraph_vector_t edges_vector;
   
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iO!O!", kwlist,
				   &n, &PyList_Type, &edges,
				   &PyBool_Type, &dir))
    return -1;

  if (edges && PyList_Check(edges)) {
    // Caller specified an edge list, so we use igraph_create
    // We have to convert the Python list to a igraph_vector_t
    if (igraphmodule_PyList_to_vector_t(edges, &edges_vector, 1, 1)) {
      igraphmodule_handle_igraph_error();
      return -1;
    }

    /*printf("Edge list:");
    for (i=0; i<n; i++)
      printf(" %d", (int)(VECTOR(edges_vector)[i]));
    printf("\n");*/
    
    if (igraph_create(&self->g, &edges_vector, (igraph_integer_t)n, (dir==Py_True))) {
      igraphmodule_handle_igraph_error();
      return -1;
    }
    
    igraph_vector_destroy(&edges_vector);
  } else {
    // No edge list was specified, let's use igraph_empty
    if (igraph_empty(&self->g, n, (dir==Py_True))) {
      igraphmodule_handle_igraph_error();
      return -1;
    }
  }
   
  return 0;
}

/** \ingroup python_interface_graph
 * \brief Formats an \c igraph.Graph object in a human-readable format.
 * 
 * This function is rather simple now, it returns the number of vertices
 * and edges in a string.
 * 
 * \return the formatted textual representation as a \c PyObject
 */
PyObject* igraphmodule_Graph_str(igraphmodule_GraphObject *self)
{
   if (igraph_is_directed(&self->g))
     return PyString_FromFormat("Directed graph (|V| = %ld, |E| = %ld)",
				(long)igraph_vcount(&self->g),
				(long)igraph_ecount(&self->g));
   else
     return PyString_FromFormat("Undirected graph (|V| = %ld, |E| = %ld)",
				(long)igraph_vcount(&self->g),
				(long)igraph_ecount(&self->g));
}

/** \ingroup python_interface_graph
 * \brief Returns the number of vertices in an \c igraph.Graph object.
 * \return the number of vertices as a \c PyObject
 * \sa igraph_vcount
 */
PyObject* igraphmodule_Graph_vcount(igraphmodule_GraphObject *self) 
{
   PyObject *result;
   result=Py_BuildValue("l", (long)igraph_vcount(&self->g));
   return result;
}

/** \ingroup python_interface_graph
 * \brief Returns the number of edges in an \c igraph.Graph object.
 * \return the number of edges as a \c PyObject
 * \sa igraph_ecount
 */
PyObject* igraphmodule_Graph_ecount(igraphmodule_GraphObject *self) 
{
   PyObject *result;
   result=Py_BuildValue("l", (long)igraph_ecount(&self->g));
   return result;
}

/** \ingroup python_interface_graph
 * \brief Checks whether an \c igraph.Graph object is directed.
 * \return \c True if the graph is directed, \c False otherwise.
 * \sa igraph_is_directed
 */
PyObject* igraphmodule_Graph_is_directed(igraphmodule_GraphObject *self) 
{
   if (igraph_is_directed(&self->g)) {
     Py_INCREF(Py_True); return Py_True;
   } else {
     Py_INCREF(Py_False); return Py_False;
   }
}

/** \ingroup python_interface_graph
 * \brief Adds vertices to an \c igraph.Graph
 * \return the extended \c igraph.Graph object
 * \sa igraph_add_vertices
 */
PyObject* igraphmodule_Graph_add_vertices(igraphmodule_GraphObject *self,
						 PyObject *args,
						 PyObject *kwds) 
{
   long n;
   
   if (!PyArg_ParseTuple(args, "l", &n)) return NULL;
   if (n<0)
     {
	// Throw an exception
	PyErr_SetString(PyExc_AssertionError, "Number of vertices to be added can't be negative.");
	return NULL;
     }
   if (igraph_add_vertices(&self->g, (igraph_integer_t)n, 0)) {
      igraphmodule_handle_igraph_error();
      return NULL;
   }
   
   Py_INCREF(self);
   
   return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Removes vertices from an \c igraph.Graph
 * \return the modified \c igraph.Graph object
 * 
 * \todo Need more error checking on vertex IDs. (igraph fails when an
 * invalid vertex ID is given)
 * \sa igraph_delete_vertices
 */
PyObject* igraphmodule_Graph_delete_vertices(igraphmodule_GraphObject *self,
						    PyObject *args,
						    PyObject *kwds)
{
   PyObject *list;
   igraph_vector_t v;
   
   if (!PyArg_ParseTuple(args, "O", &list)) return NULL;
   if (igraphmodule_PyList_to_vector_t(list, &v, 1, 0))
     {
	// something bad happened during conversion
	return NULL;
     }
   
   // do the hard work :)
   if (igraph_delete_vertices(&self->g, igraph_vss_vector(&v)))
     {
	igraphmodule_handle_igraph_error();
	igraph_vector_destroy(&v);
	return NULL;
     }

   igraph_vector_destroy(&v);
   
   Py_INCREF(self);
   
   return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Adds edges to an \c igraph.Graph
 * \return the extended \c igraph.Graph object
 * 
 * \todo Need more error checking on vertex IDs. (igraph fails when an
 * invalid vertex ID is given)
 * \sa igraph_add_edges
 */
PyObject* igraphmodule_Graph_add_edges(igraphmodule_GraphObject *self,
					      PyObject *args,
					      PyObject *kwds) 
{
   PyObject *list;
   igraph_vector_t v;

   if (!PyArg_ParseTuple(args, "O", &list)) return NULL;
   Py_INCREF(list);
   
   if (igraphmodule_PyList_to_vector_t(list, &v, 1, 1))
     {
	// something bad happened during conversion, release the
	// list reference and return immediately
	Py_DECREF(list);
	return NULL;
     }
   
   // do the hard work :)
   if (igraph_add_edges(&self->g, &v, 0)) 
     {
	igraphmodule_handle_igraph_error();
	Py_DECREF(list);
	igraph_vector_destroy(&v);
	return NULL;
     }
   
   Py_DECREF(list);
   
   Py_INCREF(self);
   
   igraph_vector_destroy(&v);
   
   return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Deletes edges from an \c igraph.Graph
 * \return the extended \c igraph.Graph object
 * 
 * \todo Need more error checking on vertex IDs. (igraph fails when an
 * invalid vertex ID is given)
 * \sa igraph_delete_edges
 */
PyObject* igraphmodule_Graph_delete_edges(igraphmodule_GraphObject *self,
						 PyObject *args,
						 PyObject *kwds) 
{
  PyObject *list;
  igraph_vector_t v;
  igraph_es_t es;
  
  if (!PyArg_ParseTuple(args, "O", &list)) return NULL;
  Py_INCREF(list);
   
  if (igraphmodule_PyList_to_vector_t(list, &v, 1, 1)) {
    /* something bad happened during conversion, release the
       list reference and return immediately */
    Py_DECREF(list);
    return NULL;
  }
   
  /* do the hard work :) */
  if (igraph_es_pairs(&es, &v, IGRAPH_DIRECTED)) {
    igraphmodule_handle_igraph_error();
    Py_DECREF(list);
    igraph_vector_destroy(&v);
    return NULL;
  }
  if (igraph_delete_edges(&self->g, es)) {
    igraphmodule_handle_igraph_error();
    Py_DECREF(list);
    igraph_es_destroy(&es);
    igraph_vector_destroy(&v);
    return NULL;
  }
   
  Py_DECREF(list);
  Py_INCREF(self);
   
  igraph_es_destroy(&es);
  igraph_vector_destroy(&v);
   
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief The degree of some vertices in an \c igraph.Graph
 * \return the degree list as a Python object
 * \sa igraph_degree
 */
PyObject* igraphmodule_Graph_degree(igraphmodule_GraphObject *self,
					   PyObject *args,
					   PyObject *kwds) 
{
   PyObject *list=NULL;
   int dtype=IGRAPH_ALL;
   igraph_bool_t input_was_list;
   PyObject *loops = Py_False;
   igraph_vector_t vids, result;
   
   char *kwlist[] = 
     {
	"vertices", "type", "loops", NULL
     }
   ;

   if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OiO!", kwlist,
				    &list, &dtype, &PyBool_Type, &loops))
	return NULL;
   
   if (dtype!=IGRAPH_ALL && dtype!=IGRAPH_OUT && dtype!=IGRAPH_IN) 
     {
	PyErr_SetString(PyExc_ValueError, "dtype should be either ALL or IN or OUT");
	return NULL;
     }
   
   Py_INCREF(loops);
   if (list)
     {
	Py_INCREF(list);
	if (igraphmodule_PyList_to_vector_t(list, &vids, 1, 0)) 
	  {
	     // something bad happened during conversion, release the
	     // list reference and return immediately
	     Py_DECREF(list);
	     Py_DECREF(loops);
	     return NULL;
	  }
	input_was_list=PyList_Check(list);
     }
   else
     {
	// no list was given, so we assume that the user wanted to
	// retrieve the degrees of all vertices
	if (igraph_vcount(&self->g)>0)
	  igraph_vector_init_seq(&vids, 0, igraph_vcount(&self->g)-1);
	else
	  igraph_vector_init(&vids, 0);
	input_was_list=1;
     }

   igraph_vector_init(&result, 0);
   if (igraph_degree(&self->g, &result, igraph_vss_vector(&vids),
		     (igraph_neimode_t)dtype, (igraph_bool_t)(loops == Py_True))) 
     {
       igraphmodule_handle_igraph_error();
       Py_DECREF(list);
       Py_DECREF(loops);
       igraph_vector_destroy(&vids);
       igraph_vector_destroy(&result);
       return NULL;
     }
   
   Py_DECREF(loops);
   if (list) {
     Py_DECREF(list);
   }
   
   if (input_was_list) 
     list=igraphmodule_vector_t_to_PyList(&result);
   else
     list=PyInt_FromLong(VECTOR(result)[0]);
   
   igraph_vector_destroy(&result);
   igraph_vector_destroy(&vids);
   
   return list;
}

/** \ingroup python_interface_graph
 * \brief The neighbors of a given vertex in an \c igraph.Graph
 * This method accepts a single vertex ID as a parameter, and returns the
 * neighbors of the given vertex in the form of an integer list. A
 * second argument may be passed as well, meaning the type of neighbors to
 * be returned (\c OUT for successors, \c IN for predecessors or \c ALL
 * for both of them). This argument is ignored for undirected graphs.
 * 
 * \return the neighbor list as a Python list object
 * \sa igraph_neighbors
 */
PyObject* igraphmodule_Graph_neighbors(igraphmodule_GraphObject *self,
					      PyObject *args,
					      PyObject *kwds) 
{
   PyObject *list;
   int dtype=IGRAPH_ALL;
   long idx;
   igraph_vector_t result;
   
   char *kwlist[] = 
     {
	"vertex", "type", NULL
     }
   ;

   if (!PyArg_ParseTupleAndKeywords(args, kwds, "l|i", kwlist,
				    &idx, &dtype))
     return NULL;
   
   if (dtype!=IGRAPH_ALL && dtype!=IGRAPH_OUT && dtype!=IGRAPH_IN) 
     {
	PyErr_SetString(PyExc_ValueError, "type should be either ALL or IN or OUT");
	return NULL;
     }
   
   igraph_vector_init(&result, 1);
   if (igraph_neighbors(&self->g, &result, idx, (igraph_neimode_t)dtype))
     {
	igraphmodule_handle_igraph_error();
	igraph_vector_destroy(&result);
	return NULL;
     }
   
   list=igraphmodule_vector_t_to_PyList(&result);
   igraph_vector_destroy(&result);
   
   return list;
}

/** \ingroup python_interface_graph
 * \brief The successors of a given vertex in an \c igraph.Graph
 * This method accepts a single vertex ID as a parameter, and returns the
 * successors of the given vertex in the form of an integer list. It
 * is equivalent to calling \c igraph.Graph.neighbors with \c type=OUT
 * 
 * \return the successor list as a Python list object
 * \sa igraph_neighbors
 */
PyObject* igraphmodule_Graph_successors(igraphmodule_GraphObject *self,
					       PyObject *args,
					       PyObject *kwds) 
{
  PyObject *list;
  long idx;
  igraph_vector_t result;
   
  char *kwlist[] = {"vertex", NULL};
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "l", kwlist, &idx))
    return NULL;
   
  igraph_vector_init(&result, 1);
  if (igraph_neighbors(&self->g, &result, idx, IGRAPH_OUT))  {
    igraphmodule_handle_igraph_error();
    igraph_vector_destroy(&result);
    return NULL;
  }
   
   list=igraphmodule_vector_t_to_PyList(&result);
   igraph_vector_destroy(&result);
   
   return list;
}

/** \ingroup python_interface_graph
 * \brief The predecessors of a given vertex in an \c igraph.Graph
 * This method accepts a single vertex ID as a parameter, and returns the
 * predecessors of the given vertex in the form of an integer list. It
 * is equivalent to calling \c igraph.Graph.neighbors with \c type=IN
 * 
 * \return the predecessor list as a Python list object
 * \sa igraph_neighbors
 */
PyObject* igraphmodule_Graph_predecessors(igraphmodule_GraphObject *self,
						 PyObject *args,
						 PyObject *kwds) 
{
  PyObject *list;
  long idx;
  igraph_vector_t result;
   
  char *kwlist[] = {"vertex", NULL};
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "l", kwlist, &idx))
    return NULL;
   
  igraph_vector_init(&result, 1);
  if (igraph_neighbors(&self->g, &result, idx, IGRAPH_IN))  {
    igraphmodule_handle_igraph_error();
    igraph_vector_destroy(&result);
    return NULL;
  }
   
   list=igraphmodule_vector_t_to_PyList(&result);
   igraph_vector_destroy(&result);
   
   return list;
}

/** \ingroup python_interface_graph
 * \brief Calculates the diameter of an \c igraph.Graph
 * This method accepts two optional parameters: the first one is
 * a boolean meaning whether to consider directed paths (and is
 * ignored for undirected graphs). The second one is only meaningful
 * in unconnected graphs: it is \c True if the longest geodesic
 * within a component should be returned and \c False if the number of
 * vertices should be returned. They both have a default value of \c False.
 * 
 * \return the diameter as a Python integer
 * \sa igraph_diameter
 */
PyObject* igraphmodule_Graph_diameter(igraphmodule_GraphObject *self,
					     PyObject *args,
					     PyObject *kwds) 
{
  PyObject *dir=NULL, *vcount_if_unconnected=NULL;
  igraph_integer_t i;
  int r;
   
  char *kwlist[] = {
    "directed", "unconn", NULL
  };

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O!O!", kwlist,
				   &PyBool_Type, &dir,
				   &PyBool_Type, &vcount_if_unconnected))
    return NULL;
  
  r=igraph_diameter(&self->g, &i, 0, 0, 0, (igraph_bool_t)(dir == Py_True),
		    (igraph_bool_t)(vcount_if_unconnected == Py_True));
  if (r) 
     {
	igraphmodule_handle_igraph_error();
	return NULL;
     }
   
   return PyInt_FromLong((long)i);
}

/** \ingroup python_interface_graph
 * \brief Generates a graph from its adjacency matrix
 * \return a reference to the newly generated Python igraph object
 * \sa igraph_adjacency
 */
PyObject* igraphmodule_Graph_Adjacency(PyTypeObject *type,
				       PyObject *args,
				       PyObject *kwds) {
  igraphmodule_GraphObject *self;
  igraph_matrix_t m;
  PyObject *matrix;
  igraph_adjacency_t mode = IGRAPH_ADJ_DIRECTED;
  
  char *kwlist[] = {"matrix", "mode", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!|i", kwlist,
				   &PyList_Type, &matrix, &mode))
    return NULL;
      
  if (igraphmodule_PyList_to_matrix_t(matrix, &m)) {
    PyErr_SetString(PyExc_TypeError, "Error while converting adjacency matrix");
    return NULL;
  }
  
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  if (self != NULL) {
    igraphmodule_Graph_init_internal(self);
    if (igraph_adjacency(&self->g, &m, mode)) {
      igraphmodule_handle_igraph_error();
      igraph_matrix_destroy(&m);
      return NULL;
    }
  }
   
  igraph_matrix_destroy(&m);
  
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Generates a graph from the Graph Atlas
 * \return a reference to the newly generated Python igraph object
 * \sa igraph_atlas
 */
PyObject* igraphmodule_Graph_Atlas(PyTypeObject *type,
				   PyObject *args) {
  long n;
  igraphmodule_GraphObject *self;
  
  if (!PyArg_ParseTuple(args, "l", &n)) return NULL;
  
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  if (self != NULL) {
    igraphmodule_Graph_init_internal(self);
    if (igraph_atlas(&self->g, (igraph_integer_t)n)) {
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  }
   
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Generates a graph based on the Barabasi-Albert model
 * This is intended to be a class method in Python, so the first argument
 * is the type object and not the Python igraph object (because we have
 * to allocate that in this method).
 * 
 * \return a reference to the newly generated Python igraph object
 * \sa igraph_barabasi_game
 */
PyObject* igraphmodule_Graph_Barabasi(PyTypeObject *type,
				      PyObject *args,
				      PyObject *kwds) 
{
  igraphmodule_GraphObject *self;
  long n, m=0;
  float power=0.0, zeroappeal=0.0;
  igraph_vector_t outseq;
  PyObject *m_obj, *outpref=Py_False, *directed=Py_False;
  
  char *kwlist[] = {"n", "m", "outpref", "directed", "power", "zeroappeal", NULL};
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "lO|OOff", kwlist,
				   &n, &m_obj, &outpref, &directed, &power, &zeroappeal))
    return NULL;
  
  if (n<0) {
    PyErr_SetString(PyExc_ValueError, "Number of vertices must be positive.");
    return NULL;
  }
  
  // let's check whether we have a constant out-degree or a list
  if (PyInt_Check(m_obj)) {
    m=PyInt_AsLong(m_obj);
    igraph_vector_init(&outseq, 0);
  } else if (PyList_Check(m_obj)) {
    if (igraphmodule_PyList_to_vector_t(m_obj, &outseq, 1, 0)) {
      // something bad happened during conversion
      return NULL;
    }
  }
  
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  if (self != NULL) {
    igraphmodule_Graph_init_internal(self);
    if (power == 0.0) {
      /* linear model */
      if (igraph_barabasi_game(&self->g, (igraph_integer_t)n,
			       (igraph_integer_t)m,
			       &outseq, PyObject_IsTrue(outpref),
			       PyObject_IsTrue(directed))) {
	igraphmodule_handle_igraph_error();
	igraph_vector_destroy(&outseq);
	return NULL;
      }
    } else {
      /* nonlinear model */
      if (igraph_nonlinear_barabasi_game(&self->g, (igraph_integer_t)n,
					 (igraph_real_t)power,
					 (igraph_integer_t)m,
					 &outseq, PyObject_IsTrue(outpref),
					 (igraph_real_t)zeroappeal,
					 PyObject_IsTrue(directed))) {
	igraphmodule_handle_igraph_error();
	igraph_vector_destroy(&outseq);
	return NULL;
      }
    }
  }
  
  igraph_vector_destroy(&outseq);
  
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Generates a graph based on the Erd�s-R�nyi model
 * \return a reference to the newly generated Python igraph object
 * \sa igraph_erdos_renyi_game
 */
PyObject* igraphmodule_Graph_Erdos_Renyi(PyTypeObject *type,
						PyObject *args,
						PyObject *kwds) 
{
  igraphmodule_GraphObject *self;
  long n, m=-1;
  double p=-1.0;
  igraph_erdos_renyi_t t;
  PyObject *loops=NULL, *directed=NULL;
  
  char *kwlist[] = {"n", "p", "m", "directed", "loops", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "l|dlO!O!", kwlist,
				   &n, &p, &m,
				   &PyBool_Type, &directed,
				   &PyBool_Type, &loops))
    return NULL;
      
  if (n<0) {
    PyErr_SetString(PyExc_ValueError, "Number of vertices must be positive.");
    return NULL;
  }
   
  if (m==-1 && p==-1.0) {
    // no density parameters were given, throw exception
    PyErr_SetString(PyExc_TypeError, "Either m or p must be given.");
    return NULL;
  }
  if (m!=-1 && p!=-1.0) {
    // both density parameters were given, throw exception
    PyErr_SetString(PyExc_TypeError, "Only one must be given from m and p.");
    return NULL;
  }
   
  t=(m==-1)?IGRAPH_ERDOS_RENYI_GNP:IGRAPH_ERDOS_RENYI_GNM;
   
  if (t==IGRAPH_ERDOS_RENYI_GNP) {
    if (p<0.0 || p>1.0) {
      // Invalid probability was given, throw exception
      PyErr_SetString(PyExc_ValueError, "p must be between 0 and 1.");
      return NULL;
    }	
  } else {
    if (m<0 || m>n*n) {
      // Invalid edge count was given, throw exception
      PyErr_SetString(PyExc_ValueError, "m must be between 0 and n^2.");
      return NULL;
    }	
  }
      
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  if (self != NULL) {
    igraphmodule_Graph_init_internal(self);
    if (igraph_erdos_renyi_game(&self->g, t, (igraph_integer_t)n,
				(igraph_real_t)((t==IGRAPH_ERDOS_RENYI_GNM)?m:p),
				(directed == Py_True),
				(loops == Py_True))) {
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  }
   
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Generates a graph based on a simple growing model with vertex types
 * \return a reference to the newly generated Python igraph object
 * \sa igraph_establishment_game
 */
PyObject* igraphmodule_Graph_Establishment(PyTypeObject *type,
					   PyObject *args,
					   PyObject *kwds) 
{
  igraphmodule_GraphObject *self;
  long n, types, k;
  PyObject *type_dist, *pref_matrix;
  PyObject *directed = Py_False;
  igraph_matrix_t pm;
  igraph_vector_t td;
  
  char *kwlist[] = {"n", "k", "type_dist", "pref_matrix", "directed", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "llO!O!|O", kwlist,
				   &n, &k, &PyList_Type, &type_dist,
				   &PyList_Type, &pref_matrix,
				   &directed))
    return NULL;
      
  if (n<=0 || k<=0) {
    PyErr_SetString(PyExc_ValueError, "Number of vertices and the amount of connection trials per step must be positive.");
    return NULL;
  }
  types = PyList_Size(type_dist);
  
  if (igraphmodule_PyList_to_matrix_t(pref_matrix, &pm)) {
    PyErr_SetString(PyExc_TypeError, "Error while converting preference matrix");
    return NULL;
  }
  if (igraph_matrix_nrow(&pm) != igraph_matrix_ncol(&pm) ||
      igraph_matrix_nrow(&pm) != types) {
    PyErr_SetString(PyExc_ValueError, "Preference matrix must have exactly the same rows and columns as the number of types");
    igraph_matrix_destroy(&pm);
    return NULL;
  }
  if (igraphmodule_PyList_to_vector_t(type_dist, &td, 1, 0)) {
    PyErr_SetString(PyExc_ValueError, "Error while converting type distribution vector");
    igraph_matrix_destroy(&pm);
    return NULL;
  }
  
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  if (self != NULL) {
    igraphmodule_Graph_init_internal(self);
    if (igraph_establishment_game(&self->g, (igraph_integer_t)n,
				  (igraph_integer_t)types,
				  (igraph_integer_t)k, &td, &pm,
				  PyObject_IsTrue(directed))) {
      igraphmodule_handle_igraph_error();
      igraph_matrix_destroy(&pm);
      igraph_vector_destroy(&td);
      return NULL;
    }
  }
   
  igraph_matrix_destroy(&pm);
  igraph_vector_destroy(&td);
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Generates a full graph
 * \return a reference to the newly generated Python igraph object
 * \sa igraph_full
 */
PyObject* igraphmodule_Graph_Full(PyTypeObject *type,
					 PyObject *args,
					 PyObject *kwds) 
{
  igraphmodule_GraphObject *self;
  long n;
  PyObject *loops=NULL, *directed=NULL;
  
  char *kwlist[] = {"n", "directed", "loops", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "l|O!O!", kwlist, &n,
				   &PyBool_Type, &directed,
				   &PyBool_Type, &loops))
    return NULL;
  
  if (n<0) {
    PyErr_SetString(PyExc_ValueError, "Number of vertices must be positive.");
    return NULL;
  }
   
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  if (self != NULL) {
    igraphmodule_Graph_init_internal(self);
    if (igraph_full(&self->g, (igraph_integer_t)n,
		    (directed == Py_True), (loops == Py_True))) {
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  }
   
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Generates a growing random graph
 * \return a reference to the newly generated Python igraph object
 * \sa igraph_growing_random_game
 */
PyObject* igraphmodule_Graph_Growing_Random(PyTypeObject *type,
						   PyObject *args,
						   PyObject *kwds) 
{
   long n, m;
   PyObject *directed=NULL, *citation=NULL;
   igraphmodule_GraphObject *self;
   
   char *kwlist[] = {"n", "m", "directed", "citation", NULL};

   if (!PyArg_ParseTupleAndKeywords(args, kwds, "ll|O!O!", kwlist, &n, &m,
				    &PyBool_Type, &directed,
				    &PyBool_Type, &citation))
    return NULL;
  
  if (n<0) {
    PyErr_SetString(PyExc_ValueError, "Number of vertices must be positive.");
    return NULL;
  }
  
  if (m<0) {
    PyErr_SetString(PyExc_ValueError, "Number of new edges per iteration must be positive.");
    return NULL;
  }
   
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  if (self != NULL) {
    igraphmodule_Graph_init_internal(self);
    if (igraph_growing_random_game(&self->g, (igraph_integer_t)n,
				   (igraph_integer_t)m, (directed == Py_True),
				   (citation == Py_True))) {
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  }
   
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Generates a star graph
 * \return a reference to the newly generated Python igraph object
 * \sa igraph_star
 */
PyObject* igraphmodule_Graph_Star(PyTypeObject *type,
					 PyObject *args,
					 PyObject *kwds) {
  long n, center=0;
  igraph_star_mode_t mode=IGRAPH_STAR_UNDIRECTED;
  igraphmodule_GraphObject *self;
  
  char *kwlist[] = {"n", "mode", "center", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "l|ll", kwlist,
				   &n, &mode, &center))
    return NULL;
  
  if (n<0) {
    PyErr_SetString(PyExc_ValueError, "Number of vertices must be positive.");
    return NULL;
  }
   
  if (center>=n || center<0) {
    PyErr_SetString(PyExc_ValueError, "Central vertex ID should be between 0 and n-1");
    return NULL;
  }
   
  if (mode!=IGRAPH_STAR_UNDIRECTED && mode!=IGRAPH_STAR_IN &&
      mode!=IGRAPH_STAR_OUT) {
    PyErr_SetString(PyExc_ValueError, "Mode should be either STAR_IN, STAR_OUT or STAR_UNDIRECTED.");
    return NULL;
  }
  
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  if (self != NULL) {
    igraphmodule_Graph_init_internal(self);
    if (igraph_star(&self->g, (igraph_integer_t)n, mode, (igraph_integer_t)center)) {
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  }
   
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Generates a regular lattice
 * \return a reference to the newly generated Python igraph object
 * \sa igraph_lattice
 */
PyObject* igraphmodule_Graph_Lattice(PyTypeObject *type,
				     PyObject *args,
				     PyObject *kwds) {
  igraph_vector_t dimvector;
  long nei=1, ndims, i;
  igraph_bool_t directed;
  igraph_bool_t mutual;
  igraph_bool_t circular;
  PyObject *o_directed=Py_False, *o_mutual=Py_True, *o_circular=Py_True;
  PyObject *o_dimvector=Py_None, *o;
  igraphmodule_GraphObject *self;
  
  char *kwlist[] = {"dim", "nei", "directed", "mutual", "circular", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!|lOOO", kwlist,
				   &PyList_Type, &o_dimvector,
				   &nei, &o_directed, &o_mutual, &o_circular))
    return NULL;

  directed=PyObject_IsTrue(o_directed);
  mutual=PyObject_IsTrue(o_mutual);
  circular=PyObject_IsTrue(o_circular);

  ndims=PyList_Size(o_dimvector);
  igraph_vector_init(&dimvector, ndims);
  for (i=0; i<ndims; i++) {
    o=PyList_GetItem(o_dimvector, i);
    if (o) {
      if (PyInt_Check(o))
	VECTOR(dimvector)[i] = (igraph_integer_t)PyInt_AsLong(o);
      else {
	PyErr_SetString(PyExc_TypeError, "Dimension list must contain integers");
	igraph_vector_destroy(&dimvector);
	return NULL;
      }
    } else {
      igraph_vector_destroy(&dimvector);
      return NULL;
    }
  }
	
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  if (self != NULL) {
    igraphmodule_Graph_init_internal(self);
    if (igraph_lattice(&self->g, &dimvector, nei, directed, mutual, circular)) {
      igraph_vector_destroy(&dimvector);
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  }
   
  igraph_vector_destroy(&dimvector);
  
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Generates a ring-shaped graph
 * \return a reference to the newly generated Python igraph object
 * \sa igraph_ring
 */
PyObject* igraphmodule_Graph_Ring(PyTypeObject *type,
					 PyObject *args,
					 PyObject *kwds) 
{
  long n;
  PyObject *directed=Py_False, *mutual=Py_False, *circular=Py_True;
  igraphmodule_GraphObject *self;
  
  char *kwlist[] = {"n", "directed", "mutual", "circular", NULL};
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "l|O!O!O!", kwlist, &n,
				   &PyBool_Type, &directed,
				   &PyBool_Type, &mutual,
				   &PyBool_Type, &circular))
    return NULL;
  
  if (n<0) {
    PyErr_SetString(PyExc_ValueError, "Number of vertices must be positive.");
    return NULL;
  }
   
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  if (self != NULL) {
    igraphmodule_Graph_init_internal(self);
    if (igraph_ring(&self->g, (igraph_integer_t)n, (directed == Py_True),
		    (mutual == Py_True), (circular == Py_True))) {
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  }
   
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Generates a tree graph where almost all vertices have an equal number of children
 * \return a reference to the newly generated Python igraph object
 * \sa igraph_tree
 */
PyObject* igraphmodule_Graph_Tree(PyTypeObject *type,
				  PyObject *args, PyObject *kwds) {
  long n, children;
  igraph_tree_mode_t mode=IGRAPH_TREE_UNDIRECTED;
  igraphmodule_GraphObject *self;
  
  char *kwlist[] = {"n", "children", "type", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "ll|l", kwlist,
				   &n, &children, &mode))
    return NULL;
  
  if (n<0) {
    PyErr_SetString(PyExc_ValueError, "Number of vertices must be positive.");
    return NULL;
  }
   
  if (mode!=IGRAPH_TREE_UNDIRECTED && mode!=IGRAPH_TREE_IN &&
      mode!=IGRAPH_TREE_OUT) {
    PyErr_SetString(PyExc_ValueError, "Mode should be either TREE_IN, TREE_OUT or TREE_UNDIRECTED.");
    return NULL;
  }
   
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  if (self != NULL) {
    igraphmodule_Graph_init_internal(self);
    if (igraph_tree(&self->g, (igraph_integer_t)n, (igraph_integer_t)children, mode)) {
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  }
   
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Generates a random graph with a given degree sequence
 * This is intended to be a class method in Python, so the first argument
 * is the type object and not the Python igraph object (because we have
 * to allocate that in this method).
 * 
 * \return a reference to the newly generated Python igraph object
 * \sa igraph_degree_sequence_game
 */
PyObject* igraphmodule_Graph_Degree_Sequence(PyTypeObject *type,
					     PyObject *args, PyObject *kwds) {
  igraphmodule_GraphObject *self;
  igraph_vector_t outseq, inseq;
  PyObject *outdeg=NULL, *indeg=NULL;
  
  char *kwlist[] = {"out", "in", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!|O!", kwlist,
				   &PyList_Type, &outdeg,
				   &PyList_Type, &indeg))
    return NULL;
  
  if (igraphmodule_PyList_to_vector_t(outdeg, &outseq, 1, 0)) {
    // something bad happened during conversion
    return NULL;
  }
  if (indeg) {
    if (igraphmodule_PyList_to_vector_t(indeg, &inseq, 1, 0)) {
      // something bad happened during conversion
      igraph_vector_destroy(&outseq);
      return NULL;
    }
  } else {
    igraph_vector_init(&inseq, 0);
  }
  
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  if (self != NULL) {
    igraphmodule_Graph_init_internal(self);
    if (igraph_degree_sequence_game(&self->g, &outseq, &inseq,
				    IGRAPH_DEGSEQ_SIMPLE)) {
      igraphmodule_handle_igraph_error();
      igraph_vector_destroy(&outseq);
      igraph_vector_destroy(&inseq);
      return NULL;
    }
  }
   
  igraph_vector_destroy(&outseq);
  igraph_vector_destroy(&inseq);
  
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Generates a graph with a given isomorphy class
 * This is intended to be a class method in Python, so the first argument
 * is the type object and not the Python igraph object (because we have
 * to allocate that in this method).
 * 
 * \return a reference to the newly generated Python igraph object
 * \sa igraph_isoclass_create
 */
PyObject* igraphmodule_Graph_Isoclass(PyTypeObject *type,
				      PyObject *args, PyObject *kwds) {
  long n, isoclass;
  PyObject *directed=NULL;
  igraphmodule_GraphObject *self;
  
  char *kwlist[] = {"n", "class", "directed", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "ii|O", kwlist,
				   &n, &isoclass, &directed))
    return NULL;

  if (n<3 || n>4) {
    PyErr_SetString(PyExc_ValueError, "Only graphs with 3 or 4 vertices are supported");
    return NULL;
  }
  
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  RC_ALLOC("Graph", self);
  
  if (self != NULL) {
    igraphmodule_Graph_init_internal(self);
    if (igraph_isoclass_create(&self->g, n, isoclass, PyObject_IsTrue(directed))) {
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  }
    
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Decides whether a graph is connected.
 * \return Py_True if the graph is connected, Py_False otherwise
 * \sa igraph_is_connected
 */
PyObject* igraphmodule_Graph_is_connected(igraphmodule_GraphObject *self,
						 PyObject *args,
						 PyObject *kwds) 
{
   char *kwlist[] = {"mode", NULL};
   igraph_connectedness_t mode=IGRAPH_STRONG;
   igraph_bool_t res;
   
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "|l", kwlist, &mode))
     return NULL;

   if (mode != IGRAPH_STRONG && mode != IGRAPH_WEAK) 
     {
	PyErr_SetString(PyExc_ValueError, "mode must be either STRONG or WEAK");
	return NULL;
     }
   
   if (igraph_is_connected(&self->g, &res, mode)) 
     {
	igraphmodule_handle_igraph_error();
	return NULL;
     }
   if (res) {
     Py_INCREF(Py_True); return Py_True;
   } else {
     Py_INCREF(Py_False); return Py_False;
   }
}

/** \ingroup python_interface_graph
 * \brief Decides whether there is an edge from a given vertex to an other one.
 * \return Py_True if the vertices are directly connected, Py_False otherwise
 * \sa igraph_are_connected
 */
PyObject* igraphmodule_Graph_are_connected(igraphmodule_GraphObject *self,
						  PyObject *args,
						  PyObject *kwds) 
{
   char *kwlist[] = {"v1", "v2", NULL};
   long v1, v2;
   igraph_bool_t res;
   
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "ll", kwlist, &v1, &v2))
     return NULL;

   if (igraph_are_connected(&self->g, (igraph_integer_t)v1, (igraph_integer_t)v2, &res))
     return NULL;
       
   if (res) {
     Py_INCREF(Py_True); return Py_True;
   } else {
     Py_INCREF(Py_False); return Py_False;
   }
}

/** \ingroup python_interface_graph
 * \brief Calculates the average path length in a graph.
 * \return the average path length as a PyObject
 * \sa igraph_average_path_length
 */
PyObject* igraphmodule_Graph_average_path_length(igraphmodule_GraphObject *self,
							PyObject *args,
							PyObject *kwds) 
{
   char *kwlist[] = {"directed", "unconn", NULL};
   PyObject *directed=Py_True, *unconn=Py_True;
   igraph_real_t res;
   
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O!O!", kwlist,
				    &PyBool_Type, &directed,
				    &PyBool_Type, &unconn))
     return NULL;

   if (igraph_average_path_length(&self->g, &res, (directed==Py_True),
				  (unconn==Py_True))) 
     {
	igraphmodule_handle_igraph_error(); return NULL;
     }
     
   return PyFloat_FromDouble(res);
}

/** \ingroup python_interface_graph
 * \brief Calculates the betweennesses of some nodes in a graph.
 * \return the betweennesses as a list (or a single float)
 * \sa igraph_betweenness
 */
PyObject* igraphmodule_Graph_betweenness(igraphmodule_GraphObject *self, PyObject *args, PyObject *kwds)
{
   char *kwlist[] = {"vertices", "directed", NULL};
   PyObject *directed=Py_True;
   PyObject *vobj=NULL, *list=NULL;
   igraph_vector_t vids;
   igraph_vector_t res;
   int return_single=0;

   if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OO", kwlist,
				    &vobj, &directed)) {
     return NULL;
   }
  
   if (vobj == NULL) 
     {
	// no vertex list was supplied
	if (igraph_vcount(&self->g)>0) 
	  {
	     if (igraph_vector_init_seq(&vids, 0, igraph_vcount(&self->g)-1)) 
	       return igraphmodule_handle_igraph_error();
	  }
	else
	  {
	     if (igraph_vector_init(&vids, 0)) return igraphmodule_handle_igraph_error();
	  }
     }
   else
     {
	if (PyInt_Check(vobj)) return_single=1;
	
	Py_INCREF(vobj);
	// vertex list was specified, convert to igraph_vector_t
	if (igraphmodule_PyList_to_vector_t(vobj, &vids, 1, 0)) {
	   Py_DECREF(vobj);
	   return NULL;
	}
	Py_DECREF(vobj);
     }

   
   if (igraph_vector_init(&res, igraph_vector_size(&vids))) return igraphmodule_handle_igraph_error();
   
   if (igraph_betweenness(&self->g, &res, igraph_vss_vector(&vids), PyObject_IsTrue(directed)))
     {
	igraphmodule_handle_igraph_error(); return NULL;
     }
   
   if (!return_single)
     list=igraphmodule_vector_t_to_float_PyList(&res);
   else
     list=PyFloat_FromDouble(VECTOR(res)[0]);
   
   igraph_vector_destroy(&res);
   igraph_vector_destroy(&vids);
   
   return list;
}

/** \ingroup python_interface_graph
 * \brief Calculates the Google PageRank value of some nodes in the graph.
 * \return the PageRank values
 * \sa igraph_pagerank
 */
PyObject* igraphmodule_Graph_pagerank(igraphmodule_GraphObject *self, PyObject *args, PyObject *kwds)
{
  char *kwlist[] = {"vertices", "directed", "niter", "eps", "damping", NULL};
  PyObject *directed=Py_True;
  PyObject *vobj=NULL, *list=NULL;
  long int niter=1000; /// @todo maybe it should be selected adaptively based on the number of vertices?
  double eps=0.001, damping=0.85;
  igraph_vector_t vids;
  igraph_vector_t res;
  int return_single=0;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OOldd", kwlist,
				   &vobj, &directed, &niter, &eps, &damping))
    return NULL;

  if (vobj == NULL) {
    // no vertex list was supplied
    if (igraph_vcount(&self->g)>0)  {
      if (igraph_vector_init_seq(&vids, 0, igraph_vcount(&self->g)-1)) 
	return igraphmodule_handle_igraph_error();
    } else {
      if (igraph_vector_init(&vids, 0)) return igraphmodule_handle_igraph_error();
    }
  } else {
    if (PyInt_Check(vobj)) return_single=1;
      
    Py_INCREF(vobj);
    // vertex list was specified, convert to igraph_vector_t
    if (igraphmodule_PyList_to_vector_t(vobj, &vids, 1, 0)) {
      Py_DECREF(vobj);
      return NULL;
    }
    Py_DECREF(vobj);
  }
   
  if (igraph_vector_init(&res, igraph_vector_size(&vids))) return igraphmodule_handle_igraph_error();
   
  if (igraph_pagerank(&self->g, &res, igraph_vss_vector(&vids),
		      PyObject_IsTrue(directed), niter, eps, damping)) {
    igraphmodule_handle_igraph_error(); return NULL;
  }
   
  if (!return_single)
    list=igraphmodule_vector_t_to_float_PyList(&res);
  else
    list=PyFloat_FromDouble(VECTOR(res)[0]);
   
  igraph_vector_destroy(&res);
  igraph_vector_destroy(&vids);
   
  return list;
}

/** \ingroup python_interface_graph
 * \brief Calculates the bibliographic coupling of some nodes in a graph.
 * \return the bibliographic coupling values in a matrix
 * \sa igraph_bibcoupling
 */
PyObject* igraphmodule_Graph_bibcoupling(igraphmodule_GraphObject *self,
						PyObject *args,
						PyObject *kwds) 
{
   char *kwlist[] = {"vertices", NULL};
   PyObject *vobj=NULL, *list=NULL;
   igraph_vector_t vids;
   igraph_matrix_t res;
   int return_single=0;
   
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &vobj))
     return NULL;

   if (vobj == NULL) 
     {
	// no vertex list was supplied
	if (igraph_vcount(&self->g)>0) 
	  {
	     if (igraph_vector_init_seq(&vids, 0, igraph_vcount(&self->g)-1)) 
	       return igraphmodule_handle_igraph_error();
	  }
	else
	  {
	     if (igraph_vector_init(&vids, 0)) return igraphmodule_handle_igraph_error();
	  }
     }
   else
     {
	if (PyInt_Check(vobj)) return_single=1;
	
	Py_INCREF(vobj);
	// vertex list was specified, convert to igraph_vector_t
	if (igraphmodule_PyList_to_vector_t(vobj, &vids, 1, 0)) {
	   Py_DECREF(vobj);
	   return NULL;
	}
	Py_DECREF(vobj);
     }

   
   if (igraph_matrix_init(&res, igraph_vector_size(&vids), igraph_vcount(&self->g)))
     return igraphmodule_handle_igraph_error();
   
   if (igraph_bibcoupling(&self->g, &res, igraph_vss_vector(&vids)))
     {
	igraphmodule_handle_igraph_error(); return NULL;
     }

   /// \todo Return a single list instead of a matrix if only one vertex was given
   list=igraphmodule_matrix_t_to_PyList(&res, IGRAPHMODULE_TYPE_INT);
   
   igraph_matrix_destroy(&res);
   igraph_vector_destroy(&vids);
   
   return list;
}

/** \ingroup python_interface_graph
 * \brief Calculates the closeness centrality of some nodes in a graph.
 * \return the closeness centralities as a list (or a single float)
 * \sa igraph_betweenness
 */
PyObject* igraphmodule_Graph_closeness(igraphmodule_GraphObject *self,
					      PyObject *args,
					      PyObject *kwds) 
{
   char *kwlist[] = {"vertices", "mode", NULL};
   PyObject *vobj=NULL, *list=NULL;
   igraph_vector_t vids;
   igraph_vector_t res;
   igraph_neimode_t mode=IGRAPH_ALL;
   int return_single=0;
   
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "|Ol", kwlist,
				    &vobj, &mode))
     return NULL;

   if (mode != IGRAPH_OUT && mode != IGRAPH_IN && mode != IGRAPH_ALL) 
     {
	PyErr_SetString(PyExc_ValueError, "mode must be one of IN, OUT or ALL");
	return NULL;
     }
   
   if (vobj == NULL) 
     {
	// no vertex list was supplied
	if (igraph_vcount(&self->g)>0) 
	  {
	     if (igraph_vector_init_seq(&vids, 0, igraph_vcount(&self->g)-1)) 
	       return igraphmodule_handle_igraph_error();
	  }
	else
	  {
	     if (igraph_vector_init(&vids, 0)) return igraphmodule_handle_igraph_error();
	  }
     }
   else
     {
	if (PyInt_Check(vobj)) return_single=1;
	
	Py_INCREF(vobj);
	// vertex list was specified, convert to igraph_vector_t
	if (igraphmodule_PyList_to_vector_t(vobj, &vids, 1, 0)) {
	   Py_DECREF(vobj);
	   return NULL;
	}
	Py_DECREF(vobj);
     }

   
   if (igraph_vector_init(&res, igraph_vector_size(&vids))) return igraphmodule_handle_igraph_error();
   
   if (igraph_closeness(&self->g, &res, igraph_vss_vector(&vids), mode))
     {
	igraphmodule_handle_igraph_error(); return NULL;
     }
   
   if (!return_single)
     list=igraphmodule_vector_t_to_float_PyList(&res);
   else
     list=PyFloat_FromDouble(VECTOR(res)[0]);
   
   igraph_vector_destroy(&res);
   igraph_vector_destroy(&vids);
   
   return list;
}

/** \ingroup python_interface_graph
 * \brief Calculates the (weakly or strongly) connected components in a graph.
 * \return a list containing the cluster ID for every vertex in the graph
 * \sa igraph_clusters
 */
PyObject* igraphmodule_Graph_clusters(igraphmodule_GraphObject *self,
					     PyObject *args,
					     PyObject *kwds) 
{
   char *kwlist[] = {"mode", NULL};
   igraph_connectedness_t mode=IGRAPH_STRONG;
   igraph_vector_t res1, res2;
   PyObject *list;
   
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "|l", kwlist, &mode))
     return NULL;

   if (mode != IGRAPH_STRONG && mode != IGRAPH_WEAK) 
     {
	PyErr_SetString(PyExc_ValueError, "mode must be either STRONG or WEAK");
	return NULL;
     }
   
   igraph_vector_init(&res1, igraph_vcount(&self->g));
   igraph_vector_init(&res2, 10);
   
   if (igraph_clusters(&self->g, &res1, &res2, mode)) 
     {
	igraphmodule_handle_igraph_error();
	igraph_vector_destroy(&res1);
	igraph_vector_destroy(&res2);
	return NULL;
     }
   
   list=igraphmodule_vector_t_to_PyList(&res1);
   igraph_vector_destroy(&res1);
   igraph_vector_destroy(&res2);
   return list;
}

/** \ingroup python_interface_copy
 * \brief Creates an exact deep copy of the graph
 * \return the copy of the graph
 */
PyObject* igraphmodule_Graph_copy(igraphmodule_GraphObject *self) {
  igraphmodule_GraphObject *result;
  igraph_t g;
  
  if (igraph_copy(&g, &self->g)) {
    igraphmodule_handle_igraph_error();
    return NULL;
  }
  
  result = (igraphmodule_GraphObject*)self->ob_type->tp_alloc(self->ob_type, 0);
  igraphmodule_Graph_init_internal(result);
  result->g=g;
  RC_ALLOC("Graph", result);
  
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Decomposes a graph into components.
 * \return a list of graph objects, each containing a copy of a component in the original graph.
 * \sa igraph_components
 */
PyObject* igraphmodule_Graph_decompose(igraphmodule_GraphObject *self,
				       PyObject *args,
				       PyObject *kwds) {
  char *kwlist[] = {"mode", "maxcompno", "minelements", NULL};
  igraph_connectedness_t mode=IGRAPH_STRONG;
  PyObject *list;
  igraphmodule_GraphObject *o;
  long maxcompno=-1, minelements=-1, n, i;
  igraph_vector_ptr_t components;
  igraph_t *g;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|lll", kwlist, &mode,
				   &maxcompno, &minelements))
    return NULL;

  if (mode != IGRAPH_STRONG && mode != IGRAPH_WEAK) {
    PyErr_SetString(PyExc_ValueError, "mode must be either STRONG or WEAK");
    return NULL;
  }
  
  igraph_vector_ptr_init(&components, 3);
  if (igraph_decompose(&self->g, &components, mode, maxcompno, minelements)) {
    igraph_vector_ptr_destroy(&components);
    igraphmodule_handle_igraph_error();
    return NULL;
  }
   
  // We have to create a separate Python igraph object for every graph returned
  n=igraph_vector_ptr_size(&components);
  list=PyList_New(n);
  for (i=0; i<n; i++) {
    g=(igraph_t*)VECTOR(components)[i];
    o = (igraphmodule_GraphObject*)self->ob_type->tp_alloc(self->ob_type, 0);
    RC_ALLOC("Graph", self);
    igraphmodule_Graph_init_internal(o);
    o->g=*g;
    PyList_SET_ITEM(list, i, (PyObject*)o);
    // reference has been transferred by PyList_SET_ITEM, no need to Py_DECREF
    //
    // we mustn't call igraph_destroy here, because it would free the vertices
    // and the edges as well, but we need them in o->g. So just call free
    igraph_free(g);
  }
  
  igraph_vector_ptr_destroy(&components);
  
  return list;
}

/** \ingroup python_interface_graph
 * \brief Calculates the cocitation scores of some nodes in a graph.
 * \return the cocitation scores in a matrix
 * \sa igraph_cocitation
 */
PyObject* igraphmodule_Graph_cocitation(igraphmodule_GraphObject *self,
					       PyObject *args,
					       PyObject *kwds) 
{
   char *kwlist[] = {"vertices", NULL};
   PyObject *vobj=NULL, *list=NULL;
   igraph_vector_t vids;
   igraph_matrix_t res;
   int return_single=0;
   
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &vobj))
     return NULL;

   if (vobj == NULL) 
     {
	// no vertex list was supplied
	if (igraph_vcount(&self->g)>0) 
	  {
	     if (igraph_vector_init_seq(&vids, 0, igraph_vcount(&self->g)-1)) 
	       return igraphmodule_handle_igraph_error();
	  }
	else
	  {
	     if (igraph_vector_init(&vids, 0)) return igraphmodule_handle_igraph_error();
	  }
     }
   else
     {
	if (PyInt_Check(vobj)) return_single=1;
	
	Py_INCREF(vobj);
	// vertex list was specified, convert to igraph_vector_t
	if (igraphmodule_PyList_to_vector_t(vobj, &vids, 1, 0)) {
	   Py_DECREF(vobj);
	   return NULL;
	}
	Py_DECREF(vobj);
     }

   
   if (igraph_matrix_init(&res, igraph_vector_size(&vids), igraph_vcount(&self->g)))
     return igraphmodule_handle_igraph_error();
   
   if (igraph_cocitation(&self->g, &res, igraph_vss_vector(&vids)))
     {
	igraphmodule_handle_igraph_error(); return NULL;
     }

   /// \todo Return a single list instead of a matrix if only one vertex was given
   list=igraphmodule_matrix_t_to_PyList(&res, IGRAPHMODULE_TYPE_INT);
   
   igraph_matrix_destroy(&res);
   igraph_vector_destroy(&vids);
   
   return list;
}

/** \ingroup python_interface_graph
 * \brief Calculates the edge betweennesses in the graph
 * \return a list containing the edge betweenness for every edge
 * \sa igraph_edge_betweenness
 */
PyObject* igraphmodule_Graph_edge_betweenness(igraphmodule_GraphObject *self,
						     PyObject *args,
						     PyObject *kwds) 
{
   char *kwlist[] = {"directed", NULL};
   igraph_vector_t res;
   PyObject *list, *directed=Py_True;
   
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O!", kwlist,
				    &PyBool_Type, &directed))
     return NULL;

   igraph_vector_init(&res, igraph_ecount(&self->g));
   
   if (igraph_edge_betweenness(&self->g, &res, (directed==Py_True)))
     {
	igraphmodule_handle_igraph_error();
	igraph_vector_destroy(&res);
	return NULL;
     }
   
   list=igraphmodule_vector_t_to_float_PyList(&res);
   igraph_vector_destroy(&res);
   return list;
}

/** \ingroup python_interface_graph
 * \brief Calculates the shortest paths from/to a given node in the graph
 * \return a list containing shortest paths from/to the given node
 * \sa igraph_get_shortest_paths
 */
PyObject* igraphmodule_Graph_get_shortest_paths(igraphmodule_GraphObject *self,
						       PyObject *args,
						       PyObject *kwds) 
{
   char *kwlist[] = {"v", "mode", NULL};
   igraph_vector_t *res;
   igraph_neimode_t mode=IGRAPH_ALL;
   long from0, i, j;
   igraph_integer_t from;
   PyObject *list, *item;
   long int no_of_nodes=igraph_vcount(&self->g);
   igraph_vector_ptr_t ptrvec;
   
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "l|l", kwlist,
				    &from0, &mode))
     return NULL;

   from=(igraph_integer_t)from0;
   
   res=(igraph_vector_t*)calloc(no_of_nodes, sizeof(igraph_vector_t));
   if (!res) 
     {
	PyErr_SetString(PyExc_MemoryError, "");
	return NULL;
     }

   if (igraph_vector_ptr_init(&ptrvec, no_of_nodes)) 
     {
       PyErr_SetString(PyExc_MemoryError, "");
       return NULL;
     }  
   
   for (i=0; i<no_of_nodes; i++)
     {
       VECTOR(ptrvec)[i]=&res[i];
       igraph_vector_init(&res[i], 5);
     }
   
   if (igraph_get_shortest_paths(&self->g, &ptrvec, from, 
				 igraph_vss_all(), mode))
     {
	igraphmodule_handle_igraph_error();
	for (j=0; j<no_of_nodes; j++) igraph_vector_destroy(&res[j]);
	free(res);
	return NULL;
     }

   list=PyList_New(no_of_nodes);
   if (!list) {
      for (j=0; j<no_of_nodes; j++) igraph_vector_destroy(&res[j]);
      free(res);
      return NULL;
   }
   
   for (i=0; i<no_of_nodes; i++) 
     {
	item=igraphmodule_vector_t_to_PyList(&res[i]);
	if (!item) 
	  {
	     Py_DECREF(list);
	     for (j=0; j<no_of_nodes; j++) igraph_vector_destroy(&res[j]);
	     free(res);
	     return NULL;
	  }
	if (PyList_SetItem(list, i, item)) 
	  {
	     Py_DECREF(list);
	     for (j=0; j<no_of_nodes; j++) igraph_vector_destroy(&res[j]);
	     free(res);
	     return NULL;
	  }
     }
   
   for (j=0; j<no_of_nodes; j++) igraph_vector_destroy(&res[j]);
   free(res);
   igraph_vector_ptr_destroy(&ptrvec);
   return list;
}

/** \ingroup python_interface_graph
 * \brief Calculates all of the shortest paths from/to a given node in the graph
 * \return a list containing shortest paths from/to the given node
 * \sa igraph_get_shortest_paths
 */
PyObject* igraphmodule_Graph_get_all_shortest_paths(igraphmodule_GraphObject *self,
						       PyObject *args,
						       PyObject *kwds) 
{
  char *kwlist[] = {"v", "mode", NULL};
  igraph_vector_ptr_t res;
  igraph_neimode_t mode=IGRAPH_ALL;
  long from0, i, j, k;
  igraph_integer_t from;
  PyObject *list, *item;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "l|l", kwlist,
				   &from0, &mode))
    return NULL;
  
  from=(igraph_integer_t)from0;
  
  if (igraph_vector_ptr_init(&res, 1)) {
    igraphmodule_handle_igraph_error();
    return NULL;
  }
  
  if (igraph_get_all_shortest_paths(&self->g, &res, NULL, from, 
				    igraph_vss_all(&self->g), mode)) {
    igraphmodule_handle_igraph_error();
    igraph_vector_ptr_destroy(&res);
    return NULL;
  }
  
  j=igraph_vector_ptr_size(&res);
  list=PyList_New(j);
  if (!list) {
    for (i=0; i<j; i++) igraph_vector_destroy(igraph_vector_ptr_e(&res, i));
    igraph_vector_ptr_destroy_all(&res);
    return NULL;
  }
   
  for (i=0; i<j; i++) {
    item=igraphmodule_vector_t_to_PyList((igraph_vector_t*)igraph_vector_ptr_e(&res, i));
    if (!item) {
      Py_DECREF(list);
      for (k=0; k<j; k++) igraph_vector_destroy(igraph_vector_ptr_e(&res, k));
      igraph_vector_ptr_destroy_all(&res);
      return NULL;
    }
    if (PyList_SetItem(list, i, item)) {
      Py_DECREF(list);
      Py_DECREF(item);
      for (k=0; k<j; k++) igraph_vector_destroy(igraph_vector_ptr_e(&res, k));
      igraph_vector_ptr_destroy_all(&res);
      return NULL;
    }
  }
  
  for (i=0; i<j; i++) igraph_vector_destroy(igraph_vector_ptr_e(&res, i));
  igraph_vector_ptr_destroy_all(&res);
  return list;
}

/** \ingroup python_interface_graph
 * \brief Calculates shortest paths in a graph.
 * \return the shortest path lengths for the given vertices
 * \sa igraph_shortest_paths
 */
PyObject* igraphmodule_Graph_shortest_paths(igraphmodule_GraphObject *self,
						   PyObject *args,
						   PyObject *kwds) 
{
   char *kwlist[] = {"vertices", "mode", NULL};
   PyObject *vobj=NULL, *list=NULL;
   igraph_vector_t vids;
   igraph_matrix_t res;
   igraph_neimode_t mode=IGRAPH_ALL;
   int return_single=0;
   
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "|Ol", kwlist, &vobj, &mode))
     return NULL;

   if (mode!=IGRAPH_IN && mode!=IGRAPH_OUT && mode!=IGRAPH_ALL) 
     {
	PyErr_SetString(PyExc_ValueError, "mode must be either IN or OUT or ALL");
	return NULL;
     }
   
   if (vobj == NULL) 
     {
	// no vertex list was supplied
	if (igraph_vcount(&self->g)>0) 
	  {
	     if (igraph_vector_init_seq(&vids, 0, igraph_vcount(&self->g)-1)) 
	       return igraphmodule_handle_igraph_error();
	  }
	else
	  {
	     if (igraph_vector_init(&vids, 0)) return igraphmodule_handle_igraph_error();
	  }
     }
   else
     {
	if (PyInt_Check(vobj)) return_single=1;
	
	Py_INCREF(vobj);
	// vertex list was specified, convert to igraph_vector_t
	if (igraphmodule_PyList_to_vector_t(vobj, &vids, 1, 0)) {
	   Py_DECREF(vobj);
	   return NULL;
	}
	Py_DECREF(vobj);
     }

   
   if (igraph_matrix_init(&res, igraph_vector_size(&vids), igraph_vcount(&self->g)))
     return igraphmodule_handle_igraph_error();
   
   if (igraph_shortest_paths(&self->g, &res, igraph_vss_vector(&vids), mode))
     {
	igraphmodule_handle_igraph_error(); return NULL;
     }

   /// \todo Return a single list instead of a matrix if only one vertex was given
   list=igraphmodule_matrix_t_to_PyList(&res, IGRAPHMODULE_TYPE_INT);
   
   igraph_matrix_destroy(&res);
   igraph_vector_destroy(&vids);
   
   return list;
}

/** \ingroup python_interface_graph
 * \brief Calculates a spanning tree for a graph
 * \return a list containing the edge betweenness for every edge
 * \sa igraph_minimum_spanning_tree_unweighted
 * \sa igraph_minimum_spanning_tree_prim
 */
PyObject* igraphmodule_Graph_spanning_tree(igraphmodule_GraphObject *self,
						  PyObject *args,
						  PyObject *kwds) 
{
  char *kwlist[] = {"weights", NULL};
  igraph_t mst;
  int err;
  igraph_vector_t ws;
  PyObject *weights=NULL;
  igraphmodule_GraphObject *result=NULL;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O!", kwlist,
				   &PyList_Type, &weights))
    return NULL;
  
  if (weights && (PyList_Size(weights)<igraph_vcount(&self->g))) {
    PyErr_SetString(PyExc_ValueError, "Weight list must have at least |V| elements (|V| = node count in the graph)");
    return NULL;
  }

  if (!weights)
    err=igraph_minimum_spanning_tree_unweighted(&self->g, &mst);
  else {
    if (igraphmodule_PyList_to_vector_t(weights, &ws, 1, 0)) return NULL;
    err=igraph_minimum_spanning_tree_prim(&self->g, &mst, &ws);
  }
   
  if (err) {
    igraphmodule_handle_igraph_error();
    if (weights) igraph_vector_destroy(&ws);
    return NULL;
  }

  result = (igraphmodule_GraphObject*)self->ob_type->tp_alloc(self->ob_type, 0);
  RC_ALLOC("Graph", result);
  
  if (result != NULL) result->g=mst;
   
  if (weights) igraph_vector_destroy(&ws);
   
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Simplifies a graph by removing loops and/or multiple edges
 * \return the simplified graph.
 * \sa igraph_simplify
 */
PyObject* igraphmodule_Graph_simplify(igraphmodule_GraphObject *self,
					     PyObject *args,
					     PyObject *kwds) 
{
   char *kwlist[] = {"multiple", "loops", NULL};
   PyObject *multiple=Py_True, *loops=Py_True;
   
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OO", kwlist,
				    &multiple, &loops))
     return NULL;

   if (igraph_simplify(&self->g, PyObject_IsTrue(multiple),
		       PyObject_IsTrue(loops)))
     {
	igraphmodule_handle_igraph_error();
	return NULL;
     }

   Py_INCREF(self);
   return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Calculates the vertex indices within the same component as a given vertex
 * \return the vertex indices in a list
 * \sa igraph_subcomponent
 */
PyObject* igraphmodule_Graph_subcomponent(igraphmodule_GraphObject *self,
					     PyObject *args,
					     PyObject *kwds) 
{
   char *kwlist[] = {"v", "mode", NULL};
   igraph_vector_t res;
   igraph_neimode_t mode=IGRAPH_ALL;
   long from0;
   igraph_real_t from;
   PyObject *list=NULL;
   
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "l|l", kwlist,
				    &from0, &mode))
     return NULL;

   if (mode != IGRAPH_OUT && mode != IGRAPH_IN && mode != IGRAPH_ALL) 
     {
	PyErr_SetString(PyExc_ValueError, "mode must be either IN, OUT or ALL");
	return NULL;
     }
   
   if (from0<0 || from0>=igraph_vcount(&self->g)) 
     {
	PyErr_SetString(PyExc_ValueError, "vertex ID must be non-negative and less than the number of edges");
	return NULL;
     }
   from=(igraph_real_t)from0;

   igraph_vector_init(&res, 0);
   if (igraph_subcomponent(&self->g, &res, from, mode))
     {
	igraphmodule_handle_igraph_error();
	igraph_vector_destroy(&res);
	return NULL;
     }

   list=igraphmodule_vector_t_to_PyList(&res);
   igraph_vector_destroy(&res);
   
   return list;
}

/** \ingroup python_interface_graph
 * \brief Rewires a graph while preserving degree distribution
 * \return the rewired graph
 * \sa igraph_rewire
 */
PyObject* igraphmodule_Graph_rewire(igraphmodule_GraphObject *self,
				    PyObject *args,
				    PyObject *kwds) {
  char *kwlist[] = {"n", "mode", NULL};
  long n=1000;
  igraph_rewiring_t mode = IGRAPH_REWIRING_SIMPLE;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ll", kwlist,
				   &n, &mode)) return NULL;
  
   if (mode!=IGRAPH_REWIRING_SIMPLE) {
     PyErr_SetString(PyExc_ValueError, "mode must be REWIRING_SIMPLE");
     return NULL;
   }
   
  if (igraph_rewire(&self->g, n, mode)) {
    igraphmodule_handle_igraph_error(); return NULL;
  }
  
  Py_INCREF(self);
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Returns a subgraph of the graph based on the given vertices
 * \return the subgraph as a new igraph object
 * \sa igraph_subgraph
 */
PyObject* igraphmodule_Graph_subgraph(igraphmodule_GraphObject *self,
					     PyObject *args,
					     PyObject *kwds) {
  char *kwlist[] = {"vertices", NULL};
  igraph_vector_t vertices;
  igraph_t sg;
  igraphmodule_GraphObject *result;
  PyObject *list;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kwlist, &list))
    return NULL;

  if (igraphmodule_PyList_to_vector_t(list, &vertices, 1, 0))
    return NULL;
  
  if (igraph_subgraph(&self->g, &sg, igraph_vss_vector(&vertices))) {
    igraphmodule_handle_igraph_error();
    igraph_vector_destroy(&vertices);
    return NULL;
  }

  result = (igraphmodule_GraphObject*)self->ob_type->tp_alloc(self->ob_type, 0);
  RC_ALLOC("Graph", result);
  if (result != NULL) result->g=sg;
  
  igraph_vector_destroy(&vertices);
  
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Calculates the graph transitivity (a.k.a. clustering coefficient)
 * \return the clustering coefficient
 * \sa igraph_transitivity
 */
PyObject* igraphmodule_Graph_transitivity(igraphmodule_GraphObject *self,
					  PyObject *args,
					  PyObject *kwds) {
  char *kwlist[] = {"type", NULL};
  igraph_transitivity_type_t type = IGRAPH_TRANSITIVITY_UNDIRECTED;
  igraph_vector_t result;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|l", kwlist, &type))
    return NULL;

  igraph_vector_init(&result, 1);
  
  if (igraph_transitivity(&self->g, &result, type)) {
    igraphmodule_handle_igraph_error();
    igraph_vector_destroy(&result);
    return NULL;
  }

  if (type == IGRAPH_TRANSITIVITY_UNDIRECTED) {
    PyObject *r = Py_BuildValue("d", (double)(VECTOR(result)[0]));
    igraph_vector_destroy(&result);
    return r;
  }
  
  igraph_vector_destroy(&result);
  Py_INCREF(Py_None);
  return Py_None;
}

/** \ingroup python_interface_graph
 * \brief Calculates the graph reciprocity
 * \return the reciprocity
 * \sa igraph_reciprocity
 */
PyObject* igraphmodule_Graph_reciprocity(igraphmodule_GraphObject *self,
					 PyObject *args, PyObject *kwds) {
  char *kwlist[] = {"ignore_loops", NULL};
  igraph_real_t result;
  PyObject *ignore_loops = Py_True;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &ignore_loops))
    return NULL;

  if (igraph_reciprocity(&self->g, &result, PyObject_IsTrue(ignore_loops))) {
    igraphmodule_handle_igraph_error();
    return NULL;
  }

  return Py_BuildValue("d", (double)result);
}

/** \ingroup python_interface_graph
 * \brief Places the vertices of a graph uniformly on a circle.
 * \return the calculated coordinates as a Python list of lists
 * \sa igraph_layout_circle
 */
PyObject* igraphmodule_Graph_layout_circle(igraphmodule_GraphObject *self,
						  PyObject *args,
						  PyObject *kwds) 
{
   igraph_matrix_t m;
   PyObject *result;
   
   if (igraph_matrix_init(&m, 1, 1)) 
     {
	igraphmodule_handle_igraph_error(); return NULL;
     }
   
   if (igraph_layout_circle(&self->g, &m))
     {
	igraph_matrix_destroy(&m);
	igraphmodule_handle_igraph_error(); return NULL;
     }
   
   result=igraphmodule_matrix_t_to_PyList(&m, IGRAPHMODULE_TYPE_FLOAT);
   
   igraph_matrix_destroy(&m);
   
   return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Places the vertices of a graph uniformly on a sphere in 3D.
 * \return the calculated coordinates as a Python list of lists
 * \sa igraph_layout_sphere
 */
PyObject* igraphmodule_Graph_layout_sphere(igraphmodule_GraphObject *self,
					   PyObject *args,
					   PyObject *kwds) 
{
   igraph_matrix_t m;
   PyObject *result;
   
   if (igraph_matrix_init(&m, 1, 1)) 
     {
	igraphmodule_handle_igraph_error(); return NULL;
     }
   
   if (igraph_layout_sphere(&self->g, &m))
     {
	igraph_matrix_destroy(&m);
	igraphmodule_handle_igraph_error(); return NULL;
     }
   
   result=igraphmodule_matrix_t_to_PyList(&m, IGRAPHMODULE_TYPE_FLOAT);
   
   igraph_matrix_destroy(&m);
   
   return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Places the vertices of a graph randomly.
 * \return the calculated coordinates as a Python list of lists
 * \sa igraph_layout_random
 */
PyObject* igraphmodule_Graph_layout_random(igraphmodule_GraphObject *self,
					   PyObject *args,
					   PyObject *kwds) 
{
   igraph_matrix_t m;
   PyObject *result;
   
   if (igraph_matrix_init(&m, 1, 1)) 
     {
	igraphmodule_handle_igraph_error(); return NULL;
     }
   
   if (igraph_layout_random(&self->g, &m))
     {
	igraph_matrix_destroy(&m);
	igraphmodule_handle_igraph_error(); return NULL;
     }
   
   result=igraphmodule_matrix_t_to_PyList(&m, IGRAPHMODULE_TYPE_FLOAT);   
   igraph_matrix_destroy(&m);
   return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Places the vertices of a graph randomly in 3D.
 * \return the calculated coordinates as a Python list of lists
 * \sa igraph_layout_random_3d
 */
PyObject* igraphmodule_Graph_layout_random_3d(igraphmodule_GraphObject *self,
					      PyObject *args,
					      PyObject *kwds) 
{
   igraph_matrix_t m;
   PyObject *result;
   
   if (igraph_matrix_init(&m, 1, 1)) 
     {
	igraphmodule_handle_igraph_error(); return NULL;
     }
   
   if (igraph_layout_random_3d(&self->g, &m))
     {
	igraph_matrix_destroy(&m);
	igraphmodule_handle_igraph_error(); return NULL;
     }
   
   result=igraphmodule_matrix_t_to_PyList(&m, IGRAPHMODULE_TYPE_FLOAT);   
   igraph_matrix_destroy(&m);
   return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Places the vertices on a plane according to the Kamada-Kawai algorithm.
 * \return the calculated coordinates as a Python list of lists
 * \sa igraph_layout_kamada_kawai
 */
PyObject* igraphmodule_Graph_layout_kamada_kawai(igraphmodule_GraphObject *self,
							PyObject *args,
							PyObject *kwds) 
{
  char *kwlist[] = {"maxiter", "sigma", "initemp", "coolexp", "kkconst", NULL};
  igraph_matrix_t m;
  long niter=1000;
  double sigma, initemp, coolexp, kkconst;
  PyObject *result;
   
  sigma=igraph_vcount(&self->g);
  kkconst=sigma*sigma; sigma=sigma/4.0;
  initemp=10.0; coolexp=0.99;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ldddd", kwlist,
				   &niter, &sigma, &initemp, &coolexp, &kkconst))
    return NULL;
  
  if (igraph_matrix_init(&m, 1, 1)) {
    igraphmodule_handle_igraph_error(); return NULL;
  }
   
  if (igraph_layout_kamada_kawai(&self->g, &m, niter, sigma, initemp, coolexp, kkconst)) {
    igraph_matrix_destroy(&m);
    igraphmodule_handle_igraph_error(); return NULL;
  }
   
  result=igraphmodule_matrix_t_to_PyList(&m, IGRAPHMODULE_TYPE_FLOAT);   
  igraph_matrix_destroy(&m);
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Places the vertices on a plane according to the Kamada-Kawai algorithm in 3D.
 * \return the calculated coordinates as a Python list of lists
 * \sa igraph_layout_kamada_kawai_3d
 */
PyObject* igraphmodule_Graph_layout_kamada_kawai_3d(igraphmodule_GraphObject *self,
						    PyObject *args,
						    PyObject *kwds) 
{
  char *kwlist[] = {"maxiter", "sigma", "initemp", "coolexp", "kkconst", NULL};
  igraph_matrix_t m;
  long niter=1000;
  double sigma, initemp, coolexp, kkconst;
  PyObject *result;
   
  sigma=igraph_vcount(&self->g);
  kkconst=sigma*sigma; sigma=sigma/4.0;
  initemp=10.0; coolexp=0.99;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ldddd", kwlist,
				   &niter, &sigma, &initemp, &coolexp, &kkconst))
    return NULL;
  
  if (igraph_matrix_init(&m, 1, 1)) {
    igraphmodule_handle_igraph_error(); return NULL;
  }
   
  if (igraph_layout_kamada_kawai_3d(&self->g, &m, niter, sigma, initemp, coolexp, kkconst)) {
    igraph_matrix_destroy(&m);
    igraphmodule_handle_igraph_error(); return NULL;
  }
   
  result=igraphmodule_matrix_t_to_PyList(&m, IGRAPHMODULE_TYPE_FLOAT);   
  igraph_matrix_destroy(&m);
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Places the vertices on a plane according to the Fruchterman-Reingold algorithm.
 * \return the calculated coordinates as a Python list of lists
 * \sa igraph_layout_fruchterman_reingold
 */
PyObject* igraphmodule_Graph_layout_fruchterman_reingold(igraphmodule_GraphObject *self,
							 PyObject *args,
							 PyObject *kwds) {
  char *kwlist[] = {"maxiter", "maxdelta", "area", "coolexp", "repulserad", NULL};
  igraph_matrix_t m;
  long niter=500;
  double maxdelta, area, coolexp, repulserad;
  PyObject *result;
   
  maxdelta=igraph_vcount(&self->g);
  area=maxdelta*maxdelta; coolexp=1.5;
  repulserad=area*maxdelta;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ldddd", kwlist,
				   &niter, &maxdelta, &area, &coolexp, &repulserad))
    return NULL;
  
  if (igraph_matrix_init(&m, 1, 1)) {
    igraphmodule_handle_igraph_error(); return NULL;
  }
   
  if (igraph_layout_fruchterman_reingold(&self->g, &m, niter, maxdelta, area, coolexp, repulserad, 0)) {
    igraph_matrix_destroy(&m);
    igraphmodule_handle_igraph_error(); return NULL;
  }
   
  result=igraphmodule_matrix_t_to_PyList(&m, IGRAPHMODULE_TYPE_FLOAT);   
  igraph_matrix_destroy(&m);
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Places the vertices on a plane according to the Fruchterman-Reingold algorithm in 3D.
 * \return the calculated coordinates as a Python list of lists
 * \sa igraph_layout_fruchterman_reingold_3d
 */
PyObject* igraphmodule_Graph_layout_fruchterman_reingold_3d(igraphmodule_GraphObject *self,
							    PyObject *args,
							    PyObject *kwds) {
  char *kwlist[] = {"maxiter", "maxdelta", "area", "coolexp", "repulserad", NULL};
  igraph_matrix_t m;
  long niter=500;
  double maxdelta, area, coolexp, repulserad;
  PyObject *result;
   
  maxdelta=igraph_vcount(&self->g);
  area=maxdelta*maxdelta; coolexp=1.5;
  repulserad=area*maxdelta;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ldddd", kwlist,
				   &niter, &maxdelta, &area, &coolexp, &repulserad))
    return NULL;
  
  if (igraph_matrix_init(&m, 1, 1)) {
    igraphmodule_handle_igraph_error(); return NULL;
  }
   
  if (igraph_layout_fruchterman_reingold_3d(&self->g, &m, niter, maxdelta, area, coolexp, repulserad, 0)) {
    igraph_matrix_destroy(&m);
    igraphmodule_handle_igraph_error(); return NULL;
  }
   
  result=igraphmodule_matrix_t_to_PyList(&m, IGRAPHMODULE_TYPE_FLOAT);   
  igraph_matrix_destroy(&m);
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Places the vertices on a plane according to the Fruchterman-Reingold grid layout algorithm.
 * \return the calculated coordinates as a Python list of lists
 * \sa igraph_layout_grid_fruchterman_reingold
 */
PyObject* igraphmodule_Graph_layout_grid_fruchterman_reingold(igraphmodule_GraphObject *self,
							      PyObject *args,
							      PyObject *kwds) {
  char *kwlist[] = {"maxiter", "maxdelta", "area", "coolexp", "repulserad", "cellsize", NULL};
  igraph_matrix_t m;
  long niter=500;
  double maxdelta, area, coolexp, repulserad, cellsize;
  PyObject *result;
   
  maxdelta=igraph_vcount(&self->g);
  area=maxdelta*maxdelta; coolexp=1.5;
  repulserad=area*maxdelta;
  cellsize=1.0; // TODO: reasonable default
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|lddddd", kwlist,
				   &niter, &maxdelta, &area, &coolexp,
				   &repulserad, &cellsize))
    return NULL;
  
  if (igraph_matrix_init(&m, 1, 1)) {
    igraphmodule_handle_igraph_error(); return NULL;
  }
   
  if (igraph_layout_grid_fruchterman_reingold(&self->g, &m, niter, maxdelta, area, coolexp, repulserad, cellsize, 0)) {
    igraph_matrix_destroy(&m);
    igraphmodule_handle_igraph_error(); return NULL;
  }
   
  result=igraphmodule_matrix_t_to_PyList(&m, IGRAPHMODULE_TYPE_FLOAT);   
  igraph_matrix_destroy(&m);
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Places the vertices of a graph according to the Large Graph Layout
 * \return the calculated coordinates as a Python list of lists
 * \sa igraph_layout_lgl
 */
PyObject* igraphmodule_Graph_layout_lgl(igraphmodule_GraphObject *self,
					PyObject *args,
					PyObject *kwds) 
{
  char *kwlist[] = {"maxiter", "maxdelta", "area", "coolexp", "repulserad", "cellsize", "proot", NULL};
  igraph_matrix_t m;
  PyObject *result;
  long maxiter=500, proot=-1;
  double maxdelta, area, coolexp, repulserad, cellsize;
   
  maxdelta=igraph_vcount(&self->g);
  area=maxdelta*maxdelta; coolexp=1.5;
  repulserad=area*maxdelta;
  cellsize=1.0; // TODO: reasonable default should be set
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ldddddl", kwlist,
				   &maxiter, &maxdelta, &area, &coolexp,
				   &repulserad, &cellsize, &proot))
    return NULL;
  
  if (igraph_matrix_init(&m, 1, 1)) {
    igraphmodule_handle_igraph_error(); return NULL;
  }
  
  if (igraph_layout_lgl(&self->g, &m, maxiter, maxdelta,
			area, coolexp, repulserad, cellsize, proot)) {
    igraph_matrix_destroy(&m);
    igraphmodule_handle_igraph_error(); return NULL;
  }
   
  result=igraphmodule_matrix_t_to_PyList(&m, IGRAPHMODULE_TYPE_FLOAT);   
  igraph_matrix_destroy(&m);
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Places the vertices of a graph according to the Reingold-Tilford
 * tree layout algorithm
 * \return the calculated coordinates as a Python list of lists
 * \sa igraph_layout_reingold_tilford
 */
PyObject* igraphmodule_Graph_layout_reingold_tilford(igraphmodule_GraphObject *self,
					PyObject *args,
					PyObject *kwds) 
{
  char *kwlist[] = {"root", NULL};
  igraph_matrix_t m;
  long int root=0;
  PyObject *result;
   
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|l", kwlist,
				   &root))
    return NULL;
  
  if (igraph_matrix_init(&m, 1, 1)) {
    igraphmodule_handle_igraph_error(); return NULL;
  }
  
  if (igraph_layout_reingold_tilford(&self->g, &m, root)) {
    igraph_matrix_destroy(&m);
    igraphmodule_handle_igraph_error(); return NULL;
  }
   
  result=igraphmodule_matrix_t_to_PyList(&m, IGRAPHMODULE_TYPE_FLOAT);   
  igraph_matrix_destroy(&m);
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Returns the adjacency matrix of a graph.
 * \return the adjacency matrix as a Python list of lists
 * \sa igraph_get_adjacency
 */
PyObject* igraphmodule_Graph_get_adjacency(igraphmodule_GraphObject *self,
						  PyObject *args,
						  PyObject *kwds) 
{
   char *kwlist[] = {"type", NULL};
   igraph_get_adjacency_t t=IGRAPH_GET_ADJACENCY_BOTH;
   igraph_matrix_t m;
   PyObject *result;
   
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "|i", kwlist, &t)) return NULL;
   
   if (t!=IGRAPH_GET_ADJACENCY_UPPER && t!=IGRAPH_GET_ADJACENCY_LOWER &&
       t!=IGRAPH_GET_ADJACENCY_BOTH)
     {
	PyErr_SetString(PyExc_ValueError, "type must be either GET_ADJACENCY_LOWER or GET_ADJACENCY_UPPER or GET_ADJACENCY_BOTH");
	return NULL;
     }
   
   if (igraph_matrix_init(&m, igraph_vcount(&self->g), igraph_vcount(&self->g))) 
     {
	igraphmodule_handle_igraph_error(); return NULL;
     }
   
   if (igraph_get_adjacency(&self->g, &m, t)) 
     {
	igraphmodule_handle_igraph_error();
	igraph_matrix_destroy(&m);
	return NULL;
     }
   
   result=igraphmodule_matrix_t_to_PyList(&m, IGRAPHMODULE_TYPE_INT);
   igraph_matrix_destroy(&m);
   return result;
}

/** \ingroup python_interface_graph
 * \brief Returns the list of edges in a graph.
 * \return the list of edges, every edge is represented by a pair
 * \sa igraph_get_edgelist
 */
PyObject* igraphmodule_Graph_get_edgelist(igraphmodule_GraphObject *self,
						 PyObject *args,
						 PyObject *kwds) 
{
   igraph_vector_t edgelist;
   PyObject *result;
   
   igraph_vector_init(&edgelist, igraph_ecount(&self->g));
   if (igraph_get_edgelist(&self->g, &edgelist, 0))
     {
	igraphmodule_handle_igraph_error();
	igraph_vector_destroy(&edgelist);
	return NULL;
     }
   
   result=igraphmodule_vector_t_to_PyList_pairs(&edgelist);
   igraph_vector_destroy(&edgelist);
   
   return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Reads an edge list from a file and creates a graph from it.
 * \return the graph
 * \sa igraph_read_graph_edgelist
 */
PyObject* igraphmodule_Graph_Read_Edgelist(PyTypeObject *type,
					   PyObject *args, PyObject *kwds) {
  igraphmodule_GraphObject *self;
  char* fname=NULL;
  FILE* f;
  PyObject *directed=Py_True;
  igraph_t g;
  
  char *kwlist[] =
  {
    "f", "directed", NULL
  };

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|O", kwlist,
				   &fname, &directed))
     return NULL;

  f=fopen(fname, "r");
  if (!f) {
    PyErr_SetString(PyExc_IOError, strerror(errno));
    return NULL;
  }
  if (igraph_read_graph_edgelist(&g, f, 0, PyObject_IsTrue(directed))) {
    igraphmodule_handle_igraph_error();
    fclose(f);
    return NULL;
  }
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  if (self != NULL) {
    RC_ALLOC("Graph", self);
    igraphmodule_Graph_init_internal(self);
    self->g=g;
  }
  fclose(f);
   
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Reads an edge list from an NCOL file and creates a graph from it.
 * \return the graph
 * \sa igraph_read_graph_ncol
 */
PyObject* igraphmodule_Graph_Read_Ncol(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  igraphmodule_GraphObject *self;
  char* fname=NULL;
  FILE* f;
  PyObject *names=Py_True, *weights=Py_True, *directed=Py_True;
  igraph_t g;
  
  char *kwlist[] =
  {
    "f", "names", "weights", "directed", NULL
  };

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|OOO", kwlist,
				   &fname, &names, &weights, &directed))
     return NULL;
      
  f=fopen(fname, "r");
  if (!f) {
    PyErr_SetString(PyExc_IOError, strerror(errno));
    return NULL;
  }
  if (igraph_read_graph_ncol(&g, f, 0, PyObject_IsTrue(names), PyObject_IsTrue(weights), PyObject_IsTrue(directed))) {
    igraphmodule_handle_igraph_error();
    fclose(f);
    return NULL;
  }
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  if (self != NULL) {
    RC_ALLOC("Graph", self);
    igraphmodule_Graph_init_internal(self);
    self->g=g;
  }
  fclose(f);
  
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Reads an edge list from an LGL file and creates a graph from it.
 * \return the graph
 * \sa igraph_read_graph_lgl
 */
PyObject* igraphmodule_Graph_Read_Lgl(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  igraphmodule_GraphObject *self;
  char* fname=NULL;
  FILE* f;
  PyObject *names=Py_True, *weights=Py_True;
  igraph_t g;
  
  char *kwlist[] =
  {
    "f", "names", "weights", NULL
  };
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|OO", kwlist,
				   &fname, &names, &weights))
    return NULL;
  
  f=fopen(fname, "r");
  if (!f) {
    PyErr_SetString(PyExc_IOError, strerror(errno));
    return NULL;
  }
  if (igraph_read_graph_lgl(&g, f, PyObject_IsTrue(names), PyObject_IsTrue(weights))) {
    igraphmodule_handle_igraph_error();
    fclose(f);
    return NULL;
  }
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  if (self != NULL) {
    RC_ALLOC("Graph", self);
    igraphmodule_Graph_init_internal(self);
    self->g=g;
  }
  fclose(f);
  
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Reads an edge list from a Pajek file and creates a graph from it.
 * \return the graph
 * \sa igraph_read_graph_pajek
 */
PyObject* igraphmodule_Graph_Read_Pajek(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  igraphmodule_GraphObject *self;
  char* fname=NULL;
  FILE* f;
  igraph_t g;
  
  char *kwlist[] =
  {
    "f", NULL
  };
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist, &fname))
    return NULL;
  
  f=fopen(fname, "r");
  if (!f) {
    PyErr_SetString(PyExc_IOError, strerror(errno));
    return NULL;
  }
  if (igraph_read_graph_pajek(&g, f)) {
    igraphmodule_handle_igraph_error();
    fclose(f);
    return NULL;
  }
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  if (self != NULL) {
    RC_ALLOC("Graph", self);
    igraphmodule_Graph_init_internal(self);
    self->g=g;
  }
  fclose(f);
  
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Reads a GraphML file and creates a graph from it.
 * \return the graph
 * \sa igraph_read_graph_graphml
 */
PyObject* igraphmodule_Graph_Read_GraphML(PyTypeObject *type,
					  PyObject *args, PyObject *kwds) {
  igraphmodule_GraphObject *self;
  char* fname=NULL;
  FILE* f;
  long int index=0;
  igraph_t g;
  
  char *kwlist[] = {"f", "index", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|i", kwlist,
				   &fname, &index))
     return NULL;

  f=fopen(fname, "r");
  if (!f) {
    PyErr_SetString(PyExc_IOError, strerror(errno));
    return NULL;
  }
  if (igraph_read_graph_graphml(&g, f, index)) {
    igraphmodule_handle_igraph_error();
    fclose(f);
    return NULL;
  }
  self = (igraphmodule_GraphObject*)type->tp_alloc(type, 0);
  if (self != NULL) {
    RC_ALLOC("Graph", self);
    igraphmodule_Graph_init_internal(self);
    self->g=g;
  }
  fclose(f);
   
  return (PyObject*)self;
}

/** \ingroup python_interface_graph
 * \brief Writes the edge list to a file
 * \return none
 * \sa igraph_write_graph_edgelist
 */
PyObject* igraphmodule_Graph_write_edgelist(igraphmodule_GraphObject *self,
						   PyObject *args,
						   PyObject *kwds)
{
  char* fname=NULL;
  FILE* f;
  
  char *kwlist[] =
  {
    "f", NULL
  };

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist, &fname))
     return NULL;
      
  f=fopen(fname, "w");
  if (!f) {
    PyErr_SetString(PyExc_IOError, strerror(errno));
    return NULL;
  }
  if (igraph_write_graph_edgelist(&self->g, f))
  {
    igraphmodule_handle_igraph_error();
    fclose(f);
    return NULL;
  }
  fclose(f);
  
  Py_RETURN_NONE;
}

/** \ingroup python_interface_graph
 * \brief Writes the edge list to a file in .ncol format
 * \return none
 * \sa igraph_write_graph_ncol
 */
PyObject* igraphmodule_Graph_write_ncol(igraphmodule_GraphObject *self,
					       PyObject *args,
					       PyObject *kwds)
{
  char* fname=NULL;
  char* names="name";
  char* weights="weight";
  FILE* f;
  
  char *kwlist[] =
  {
    "f", "names", "weights", NULL
  };

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|zz", kwlist,
				   &fname, &names, &weights))
     return NULL;

  f=fopen(fname, "w");
  if (!f) {
    PyErr_SetString(PyExc_IOError, strerror(errno));
    return NULL;
  }
  if (igraph_write_graph_ncol(&self->g, f, names, weights))
  {
    igraphmodule_handle_igraph_error();
    fclose(f);
    return NULL;
  }
  fclose(f);
  
  Py_RETURN_NONE;
}

/** \ingroup python_interface_graph
 * \brief Writes the edge list to a file in .lgl format
 * \return none
 * \sa igraph_write_graph_lgl
 */
PyObject* igraphmodule_Graph_write_lgl(igraphmodule_GraphObject *self,
					      PyObject *args,
					      PyObject *kwds)
{
  char* fname=NULL;
  char* names="name";
  char* weights="weight";
  PyObject* isolates=Py_True;
  FILE* f;
  
  char *kwlist[] =
  {
    "f", "names", "weights", "isolates", NULL
  };

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|zzO", kwlist,
				   &fname, &names, &weights, &isolates))
     return NULL;

  f=fopen(fname, "w");
  if (!f) {
    PyErr_SetString(PyExc_IOError, strerror(errno));
    return NULL;
  }
  if (igraph_write_graph_lgl(&self->g, f, names, weights,
			     PyObject_IsTrue(isolates)))
  {
    igraphmodule_handle_igraph_error();
    fclose(f);
    return NULL;
  }
  fclose(f);
  
  Py_RETURN_NONE;
}

/** \ingroup python_interface_graph
 * \brief Writes the graph to a GraphML file
 * \return none
 * \sa igraph_write_graph_graphml
 */
PyObject* igraphmodule_Graph_write_graphml(igraphmodule_GraphObject *self,
					   PyObject *args,
					   PyObject *kwds)
{
  char* fname=NULL;
  FILE* f;
  
  char *kwlist[] =
  {
    "f", NULL
  };

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist, &fname))
     return NULL;
      
  f=fopen(fname, "w");
  if (!f) {
    PyErr_SetString(PyExc_IOError, strerror(errno));
    return NULL;
  }
  if (igraph_write_graph_graphml(&self->g, f))
  {
    igraphmodule_handle_igraph_error();
    fclose(f);
    return NULL;
  }
  fclose(f);
  
  Py_RETURN_NONE;
}

/** \ingroup python_interface_graph
 * \brief Calculates the isomorphy class of a graph or its subgraph
 * \sa igraph_isoclass, igraph_isoclass_subgraph
 */
PyObject* igraphmodule_Graph_isoclass(igraphmodule_GraphObject *self,
				      PyObject* args,
				      PyObject* kwds) {
  int isoclass = 0, n;
  PyObject* vids = 0;
  char *kwlist[] = {"vertices", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O!", kwlist, &PyList_Type, &vids))
     return NULL;
  
  if (vids) {
    /* vertex list was passed, check its length */
    n=PyList_Size(vids);
  } else {
    n=igraph_vcount(&self->g);
  }
  
  if (n < 3 || n > 4) {
    PyErr_SetString(PyExc_ValueError, "Graph or subgraph must have 3 or 4 vertices.");
    return NULL;
  }
  
  if (vids) {
    igraph_vector_t vidsvec;
    if (igraphmodule_PyList_to_vector_t(vids, &vidsvec, 1, 0)) {
      PyErr_SetString(PyExc_ValueError, "Error while converting PyList to igraph_vector_t");
      return NULL;
    }
    if (igraph_isoclass_subgraph(&self->g, &vidsvec, &isoclass)) {
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  } else {
    if (igraph_isoclass(&self->g, &isoclass)) {
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  }
  
  return PyInt_FromLong((long)isoclass);
}

/** \ingroup python_interface_graph
 * \brief Determines whether the graph is isomorphic to another graph
 * \sa igraph_isoclass
 */
PyObject* igraphmodule_Graph_isomorphic(igraphmodule_GraphObject *self, PyObject *args, PyObject *kwds) {
  igraph_bool_t result = 0;
  PyObject *o;
  igraphmodule_GraphObject *other;
  char *kwlist[] = {"other", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!", kwlist, &igraphmodule_GraphType, &o))
    return NULL;
  other=(igraphmodule_GraphObject*)o;
  
  if (igraph_vcount(&self->g) < 3 || igraph_vcount(&self->g) > 4) {
    PyErr_SetString(PyExc_ValueError, "Graph must have 3 or 4 vertices.");
    return NULL;
  }
  if (igraph_vcount(&other->g) < 3 || igraph_vcount(&other->g) > 4) {
    PyErr_SetString(PyExc_ValueError, "Graph must have 3 or 4 vertices.");
    return NULL;
  }
  
  if (igraph_isomorphic(&self->g, &other->g, &result)) {
    igraphmodule_handle_igraph_error();
    return NULL;
  }
  
  if (result) Py_RETURN_TRUE;
  Py_RETURN_FALSE;
}

/** \ingroup python_interface_graph
 * \brief Returns the number of graph attributes
 */
int igraphmodule_Graph_attribute_count(igraphmodule_GraphObject* self) {
  return PyDict_Size(((PyObject**)self->g.attr)[ATTRHASH_IDX_GRAPH]);
}

/** \ingroup python_interface_graph
 * \brief Returns the corresponding value to a given attribute in the graph
 */
PyObject* igraphmodule_Graph_get_attribute(igraphmodule_GraphObject* self,
					   PyObject* s) {
  PyObject* result;
  
  result=PyDict_GetItem(((PyObject**)self->g.attr)[ATTRHASH_IDX_GRAPH], s);
  if (result) {
    Py_INCREF(result);
    return result;
  }
  
  /* result is NULL, check whether there was an error */
  if (!PyErr_Occurred())
    PyErr_SetString(PyExc_KeyError, "Attribute does not exist");
  return NULL;
}

/** \ingroup python_interface_graph
 * \brief Sets the corresponding value of a given attribute in the graph
 * \param self the graph object
 * \param k the attribute name to be set
 * \param v the value to be set
 * \return 0 if everything's ok, -1 in case of error
 */
int igraphmodule_Graph_set_attribute(igraphmodule_GraphObject* self, PyObject* k, PyObject* v) {
  if (v == NULL)
    return PyDict_DelItem(((PyObject**)self->g.attr)[ATTRHASH_IDX_GRAPH], k);
  Py_INCREF(v);
  if (PyDict_SetItem(((PyObject**)self->g.attr)[ATTRHASH_IDX_GRAPH], k, v) == -1) {
    Py_DECREF(v);
    return -1;
  }
  return 0;
}

/** \ingroup python_interface_graph
 * \brief Returns the attribute list of the graph
 */
PyObject* igraphmodule_Graph_attributes(igraphmodule_GraphObject* self) {
  return PyDict_Keys(((PyObject**)self->g.attr)[ATTRHASH_IDX_GRAPH]);
}

/** \ingroup python_interface_graph
 * \brief Returns the attribute list of the graph's vertices
 */
PyObject* igraphmodule_Graph_vertex_attributes(igraphmodule_GraphObject* self) {
  return PyDict_Keys(((PyObject**)self->g.attr)[ATTRHASH_IDX_VERTEX]);
}

/** \ingroup python_interface_graph
 * \brief Returns the attribute list of the graph's edges
 */
PyObject* igraphmodule_Graph_edge_attributes(igraphmodule_GraphObject* self) {
  return PyDict_Keys(((PyObject**)self->g.attr)[ATTRHASH_IDX_EDGE]);
}

/** \ingroup python_interface_graph
 * \brief Returns the vertex sequence of the graph
 */
PyObject* igraphmodule_Graph_get_vertices(igraphmodule_GraphObject* self, void* closure) {
  if (self->vseq==NULL) {
    self->vseq=igraphmodule_VertexSeq_New(self);
  }
  Py_INCREF(self->vseq);
  return self->vseq;
}

/** \ingroup python_interface_graph
 * \brief Returns the edge sequence of the graph
 */
PyObject* igraphmodule_Graph_get_edges(igraphmodule_GraphObject* self, void* closure) {
  if (self->eseq==NULL) {
    self->eseq=igraphmodule_EdgeSeq_New(self);
  }
  Py_INCREF(self->eseq);
  return self->eseq;
}

/** \ingroup python_interface_graph
 * \brief Creates the disjoint union of two graphs (operator version)
 */
PyObject* igraphmodule_Graph_disjoint_union(igraphmodule_GraphObject* self, PyObject* other) {
  PyObject *it;
  igraphmodule_GraphObject *o, *result;
  igraph_t g;
  
  /* Did we receive an iterable? */
  it=PyObject_GetIter(other);
  if (it) {
    /* Get all elements, store the graphs in an igraph_vector_ptr */
    igraph_vector_ptr_t gs;
    if (igraphmodule_PyIter_to_vector_ptr_t(it, &gs)) {
      Py_DECREF(it);
      return NULL;
    }
    Py_DECREF(it);
    
    /* Create disjoint union */
    if (igraph_disjoint_union_many(&g, &gs)) {
      igraph_vector_ptr_destroy(&gs);
      igraphmodule_handle_igraph_error();
      return NULL;
    }
    
    igraph_vector_ptr_destroy(&gs);
  } else {
    PyErr_Clear();
    if (!PyObject_TypeCheck(other, &igraphmodule_GraphType)) {
      Py_INCREF(Py_NotImplemented);
      return Py_NotImplemented;
    }
    o=(igraphmodule_GraphObject*)other;
  
    if (igraph_disjoint_union(&g, &self->g, &o->g)) {
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  }
  
  result = (igraphmodule_GraphObject*)self->ob_type->tp_alloc(self->ob_type, 0);
  RC_ALLOC("Graph", result);
  if (result != NULL) {
    /* this is correct as long as attributes are not copied by the
     * operator. if they are copied, the initialization should not empty
     * the attribute hashes */
    igraphmodule_Graph_init_internal(result);
    result->g=g;
  }
  
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Creates the union of two graphs (operator version)
 */
PyObject* igraphmodule_Graph_union(igraphmodule_GraphObject* self, PyObject* other) {
  PyObject *it;
  igraphmodule_GraphObject *o, *result;
  igraph_t g;
  
  /* Did we receive an iterable? */
  it=PyObject_GetIter(other);
  if (it) {
    /* Get all elements, store the graphs in an igraph_vector_ptr */
    igraph_vector_ptr_t gs;
    if (igraphmodule_PyIter_to_vector_ptr_t(it, &gs)) {
      Py_DECREF(it);
      return NULL;
    }
    Py_DECREF(it);
    
    /* Create union */
    if (igraph_union_many(&g, &gs)) {
      igraph_vector_ptr_destroy(&gs);
      igraphmodule_handle_igraph_error();
      return NULL;
    }
    
    igraph_vector_ptr_destroy(&gs);
  } else {
    PyErr_Clear();
    if (!PyObject_TypeCheck(other, &igraphmodule_GraphType)) {
      Py_INCREF(Py_NotImplemented);
      return Py_NotImplemented;
    }
    o=(igraphmodule_GraphObject*)other;
  
    if (igraph_union(&g, &self->g, &o->g)) {
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  }
  
  result = (igraphmodule_GraphObject*)self->ob_type->tp_alloc(self->ob_type, 0);
  RC_ALLOC("Graph", result);
  if (result != NULL) {
    /* this is correct as long as attributes are not copied by the
     * operator. if they are copied, the initialization should not empty
     * the attribute hashes */
    igraphmodule_Graph_init_internal(result);
    result->g=g;
  }
  
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Creates the intersection of two graphs (operator version)
 */
PyObject* igraphmodule_Graph_intersection(igraphmodule_GraphObject* self, PyObject* other) {
  PyObject *it;
  igraphmodule_GraphObject *o, *result;
  igraph_t g;
  
  /* Did we receive an iterable? */
  it=PyObject_GetIter(other);
  if (it) {
    /* Get all elements, store the graphs in an igraph_vector_ptr */
    igraph_vector_ptr_t gs;
    if (igraphmodule_PyIter_to_vector_ptr_t(it, &gs)) {
      Py_DECREF(it);
      return NULL;
    }
    Py_DECREF(it);
    
    /* Create union */
    if (igraph_intersection_many(&g, &gs)) {
      igraph_vector_ptr_destroy(&gs);
      igraphmodule_handle_igraph_error();
      return NULL;
    }
    
    igraph_vector_ptr_destroy(&gs);
  } else {
    PyErr_Clear();
    if (!PyObject_TypeCheck(other, &igraphmodule_GraphType)) {
      Py_INCREF(Py_NotImplemented);
      return Py_NotImplemented;
    }
    o=(igraphmodule_GraphObject*)other;
  
    if (igraph_intersection(&g, &self->g, &o->g)) {
      igraphmodule_handle_igraph_error();
      return NULL;
    }
  }
  
  result = (igraphmodule_GraphObject*)self->ob_type->tp_alloc(self->ob_type, 0);
  RC_ALLOC("Graph", result);
  if (result != NULL) {
    /* this is correct as long as attributes are not copied by the
     * operator. if they are copied, the initialization should not empty
     * the attribute hashes */
    igraphmodule_Graph_init_internal(result);
    result->g=g;
  }
  
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Creates the difference of two graphs (operator version)
 */
PyObject* igraphmodule_Graph_difference(igraphmodule_GraphObject* self, PyObject* other) {
  igraphmodule_GraphObject *o, *result;
  igraph_t g;
  
  if (!PyObject_TypeCheck(other, &igraphmodule_GraphType)) {
    Py_INCREF(Py_NotImplemented);
    return Py_NotImplemented;
  }
  o=(igraphmodule_GraphObject*)other;
  
  if (igraph_difference(&g, &self->g, &o->g)) {
    igraphmodule_handle_igraph_error();
    return NULL;
  }
  
  result = (igraphmodule_GraphObject*)self->ob_type->tp_alloc(self->ob_type, 0);
  RC_ALLOC("Graph", result);
  if (result != NULL) {
    /* this is correct as long as attributes are not copied by the
     * operator. if they are copied, the initialization should not empty
     * the attribute hashes */
    igraphmodule_Graph_init_internal(result);
    result->g=g;
  }
  
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Creates the complementer of a graph
 */
PyObject* igraphmodule_Graph_complementer(igraphmodule_GraphObject* self, PyObject* args) {
  igraphmodule_GraphObject *result;
  PyObject *o = Py_True;
  igraph_t g;
  
  if (!PyArg_ParseTuple(args, "|O", &o)) return NULL;
  if (igraph_complementer(&g, &self->g, PyObject_IsTrue(o))) {
    igraphmodule_handle_igraph_error();
    return NULL;
  }
  
  result = (igraphmodule_GraphObject*)self->ob_type->tp_alloc(self->ob_type, 0);
  RC_ALLOC("Graph", result);
  if (result != NULL) {
    /* this is correct as long as attributes are not copied by the
     * operator. if they are copied, the initialization should not empty
     * the attribute hashes */
    igraphmodule_Graph_init_internal(result);
    result->g=g;
  }
  
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Creates the complementer of a graph (operator version)
 */
PyObject* igraphmodule_Graph_complementer_op(igraphmodule_GraphObject* self) {
  igraphmodule_GraphObject *result;
  igraph_t g;
  
  if (igraph_complementer(&g, &self->g, 0)) {
    igraphmodule_handle_igraph_error();
    return NULL;
  }
  
  result = (igraphmodule_GraphObject*)self->ob_type->tp_alloc(self->ob_type, 0);
  RC_ALLOC("Graph", result);
  if (result != NULL) {
    /* this is correct as long as attributes are not copied by the
     * operator. if they are copied, the initialization should not empty
     * the attribute hashes */
    igraphmodule_Graph_init_internal(result);
    result->g=g;
  }
  
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Creates the composition of two graphs
 */
PyObject* igraphmodule_Graph_compose(igraphmodule_GraphObject* self, PyObject* other) {
  igraphmodule_GraphObject *o, *result;
  igraph_t g;
  
  if (!PyObject_TypeCheck(other, &igraphmodule_GraphType)) {
    Py_INCREF(Py_NotImplemented);
    return Py_NotImplemented;
  }
  o=(igraphmodule_GraphObject*)other;
  
  if (igraph_compose(&g, &self->g, &o->g)) {
    igraphmodule_handle_igraph_error();
    return NULL;
  }
  
  result = (igraphmodule_GraphObject*)self->ob_type->tp_alloc(self->ob_type, 0);
  RC_ALLOC("Graph", result);
  if (result != NULL) {
    /* this is correct as long as attributes are not copied by the
     * operator. if they are copied, the initialization should not empty
     * the attribute hashes */
    igraphmodule_Graph_init_internal(result);
    result->g=g;
  }
  
  return (PyObject*)result;
}

/** \ingroup python_interface_graph
 * \brief Conducts a breadth first search (BFS) on the graph
 */
PyObject* igraphmodule_Graph_bfs(igraphmodule_GraphObject* self, PyObject* args, PyObject* kwds) {
  char *kwlist[] = {"vid", "mode", NULL};
  long vid;
  PyObject *l1, *l2, *l3, *result;
  igraph_neimode_t mode = IGRAPH_OUT;
  igraph_vector_t vids;
  igraph_vector_t layers;
  igraph_vector_t parents;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "i|i", kwlist, &vid, &mode))
    return NULL;
  if (vid < 0 || vid>igraph_vcount(&self->g)) {
    PyErr_SetString(PyExc_ValueError, "invalid vertex id");
    return NULL;
  }
  
  if (igraph_vector_init(&vids, igraph_vcount(&self->g))) {
    PyErr_SetString(PyExc_MemoryError, "not enough memory");
  }
  if (igraph_vector_init(&layers, igraph_vcount(&self->g))) {
    PyErr_SetString(PyExc_MemoryError, "not enough memory");
  }
  if (igraph_vector_init(&parents, igraph_vcount(&self->g))) {
    PyErr_SetString(PyExc_MemoryError, "not enough memory");
  }
  if (igraph_bfs(&self->g, (igraph_integer_t)vid, mode, &vids, &layers, &parents)) {
    igraphmodule_handle_igraph_error();
    return NULL;
  }
  l1=igraphmodule_vector_t_to_PyList(&vids);
  l2=igraphmodule_vector_t_to_PyList(&layers);
  l3=igraphmodule_vector_t_to_PyList(&parents);
  if (l1 && l2 && l3)
    result=Py_BuildValue("(OOO)", l1, l2, l3);
  else result=NULL;
  igraph_vector_destroy(&vids); igraph_vector_destroy(&layers);
  igraph_vector_destroy(&parents); return result;
}

/** \ingroup python_interface_graph
 * \brief Constructs a breadth first search (BFS) iterator of the graph
 */
PyObject* igraphmodule_Graph_bfsiter(igraphmodule_GraphObject* self, PyObject* args, PyObject* kwds) {
  char *kwlist[] = {"vid", "mode", "advanced", NULL};
  PyObject *root, *adv = Py_False;
  igraph_neimode_t mode = IGRAPH_OUT;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|iO", kwlist, &root, &mode, &adv))
    return NULL;
  
  return igraphmodule_BFSIter_new(self, root, mode, PyObject_IsTrue(adv));
}

/** \defgroup python_interface_internal Internal functions
 * \ingroup python_interface */

/** \ingroup python_interface_internal
 * \brief Returns the encapsulated igraph graph as a PyCObject
 * \return a new PyCObject
 */
PyObject* igraphmodule_Graph___graph_as_cobject__(igraphmodule_GraphObject *self,
						 PyObject *args,
						 PyObject *kwds) 
{
  /*
  char *kwlist[] = {"ref", NULL};
  PyObject *incref=Py_True;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &incref)) return NULL;
  if (PyObject_IsTrue(incref)) Py_INCREF(self);
  */
  
  return PyCObject_FromVoidPtr((void*)&self->g, NULL);
}


/** \ingroup python_interface_internal
 * \brief Registers a destructor to be called when the object is destroyed
 * \return the previous destructor (if any)
 * Unimplemented.
 */
PyObject* igraphmodule_Graph___register_destructor__(igraphmodule_GraphObject *self,
							    PyObject *args,
							    PyObject *kwds) 
{
  char *kwlist[] = {"destructor", NULL};
  PyObject *destructor = NULL, *result;
  
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kwlist, &destructor)) return NULL;
  
  if (!PyCallable_Check(destructor)) {
    PyErr_SetString(PyExc_TypeError, "The destructor must be callable!");
    return NULL;
  }
  
  result=self->destructor;
  self->destructor=destructor;
  Py_INCREF(self->destructor);

  if (!result) Py_RETURN_NONE;
  
  return result;
}

/** \ingroup python_interface_graph
 * This structure is the collection of functions necessary to implement
 * the graph as a mapping (i.e. to allow the retrieval and setting of
 * igraph attributes in Python as if it were of a Python mapping type)
 */
PyMappingMethods igraphmodule_Graph_as_mapping = {
  // returns the number of graph attributes
  (inquiry)igraphmodule_Graph_attribute_count,
  // returns an attribute by name
  (binaryfunc)igraphmodule_Graph_get_attribute,
  // sets an attribute by name
  (objobjargproc)igraphmodule_Graph_set_attribute
};

/** \ingroup python_interface
 * \brief Collection of methods to allow numeric operators to be used on the graph
 */
PyNumberMethods igraphmodule_Graph_as_number = {
    (binaryfunc)igraphmodule_Graph_disjoint_union, /* nb_add */
    (binaryfunc)igraphmodule_Graph_difference,	/*nb_subtract*/
    0,	/*nb_multiply*/
    0,	/*nb_divide*/
    0,	/*nb_remainder*/
    0,	/*nb_divmod*/
    0,	/*nb_power*/
    0,	/*nb_negative*/
    0,	/*nb_positive*/
    0,	/*nb_absolute*/
    0,	/*nb_nonzero*/
    (unaryfunc)igraphmodule_Graph_complementer_op,	/*nb_invert*/
    0,	/*nb_lshift*/
    0,	/*nb_rshift*/
    (binaryfunc)igraphmodule_Graph_intersection, /*nb_and*/
    0,	/*nb_xor*/
    (binaryfunc)igraphmodule_Graph_union,	 /*nb_or*/
    0,	/*nb_coerce*/
    0,	/*nb_int*/
    0,	/*nb_long*/
    0,	/*nb_float*/
    0,	/*nb_oct*/
    0, 	/*nb_hex*/
    0,	/*nb_inplace_add*/
    0,	/*nb_inplace_subtract*/
    0,	/*nb_inplace_multiply*/
    0,	/*nb_inplace_divide*/
    0,	/*nb_inplace_remainder*/
    0,	/*nb_inplace_power*/
    0,	/*nb_inplace_lshift*/
    0,	/*nb_inplace_rshift*/
    0,	/*nb_inplace_and*/
    0,	/*nb_inplace_xor*/
    0,	/*nb_inplace_or*/
};

/** \ingroup python_interface_graph
 * Python type object referencing the methods Python calls when it performs various operations on an igraph (creating, printing and so on)
 */
PyTypeObject igraphmodule_GraphType = {
  PyObject_HEAD_INIT(NULL)
  0,                                        /* ob_size */
  "igraph.Graph",                           /* tp_name */
  sizeof(igraphmodule_GraphObject),         /* tp_basicsize */
  0,                                        /* tp_itemsize */
  (destructor)igraphmodule_Graph_dealloc,   /* tp_dealloc */
  0,                                        /* tp_print */
  0,                                        /* tp_getattr */
  0,                                        /* tp_setattr */
  0,                                        /* tp_compare */
  0,                                        /* tp_repr */
  &igraphmodule_Graph_as_number,            /* tp_as_number */
  0,                                        /* tp_as_sequence */
  &igraphmodule_Graph_as_mapping,           /* tp_as_mapping */
  0,                                        /* tp_hash */
  0,                                        /* tp_call */
  (reprfunc)igraphmodule_Graph_str,         /* tp_str */
  0,                                        /* tp_getattro */
  0,                                        /* tp_setattro */
  0,                                        /* tp_as_buffer */
  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC, /* tp_flags */
  "Class representing a graph in the igraph library.",                    /* tp_doc */
  (traverseproc)igraphmodule_Graph_traverse,/* tp_traverse */
  (inquiry)igraphmodule_Graph_clear,        /* tp_clear */
  0,                                        /* tp_richcompare */
  offsetof(igraphmodule_GraphObject, weakreflist), /* tp_weaklistoffset */
  0,                                        /* tp_iter */
  0,                                        /* tp_iternext */
  0,                                        /* tp_methods */
  0,                                        /* tp_members */
  0,                                        /* tp_getset */
  0,                                        /* tp_base */
  0,                                        /* tp_dict */
  0,                                        /* tp_descr_get */
  0,                                        /* tp_descr_set */
  0,                                        /* tp_dictoffset */
  (initproc)igraphmodule_Graph_init,        /* tp_init */
  0,                                        /* tp_alloc */
  igraphmodule_Graph_new,                   /* tp_new */
  0,                                        /* tp_free */
};
