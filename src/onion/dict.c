/*
	Onion HTTP server library
	Copyright (C) 2010 David Moreno Montero

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation; either
	version 3.0 of the License, or (at your option) any later version.

	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public
	License along with this library; if not see <http://www.gnu.org/licenses/>.
	*/

#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>

#include "log.h"
#include "dict.h"
#include "types_internal.h"
#include "codecs.h"
#include "block.h"

/// @private
typedef struct onion_dict_node_data_t{
	const char *key;
	const void *value;
	short int flags;
}onion_dict_node_data;

/**
 * @short Node for the tree.
 * @memberof onion_dict_t
 * 
 * Its implemented as a AA Tree (http://en.wikipedia.org/wiki/AA_tree)
 */
typedef struct onion_dict_node_t{
	onion_dict_node_data data;
	int level; 
	struct onion_dict_node_t *left;
	struct onion_dict_node_t *right;
}onion_dict_node;

static void onion_dict_node_data_free(onion_dict_node_data *dict);
static void onion_dict_set_node_data(onion_dict_node_data *data, const char *key, const void *value, int flags);
static onion_dict_node *onion_dict_node_new(const char *key, const void *value, int flags);

/**
 * @memberof onion_dict_t
 * Initializes the basic tree with all the structure in place, but empty.
 */
onion_dict *onion_dict_new(){
	onion_dict *dict=malloc(sizeof(onion_dict));
	memset(dict,0,sizeof(onion_dict));
#ifdef HAVE_PTHREADS
	pthread_rwlock_init(&dict->lock, NULL);
	pthread_mutex_init(&dict->refmutex, NULL);
#endif
	dict->refcount=1;
	return dict;
}

/**
 * @short Creates a duplicate of the dict
 * @memberof onion_dict_t
 * 
 * Its actually the same, but with refcount increased, so future frees will free the dict
 * only on latest one.
 * 
 * Any change on one dict is made also on the other one, as well as rwlock... This is usefull on a multhreaded
 * environment so that multiple threads cna have the same dict and free it when not in use anymore.
 */
onion_dict *onion_dict_dup(onion_dict *dict){
#ifdef HAVE_PTHREADS
	pthread_mutex_lock(&dict->refmutex);
#endif
	dict->refcount++;
	//ONION_DEBUG0("Dup %p, refcount %d",dict, dict->refcount);
#ifdef HAVE_PTHREADS
	pthread_mutex_unlock(&dict->refmutex);
#endif
	return dict;
}

void onion_dict_hard_dup_helper(onion_dict *dict, const char *key, const void *value, int flags){
	if (flags&OD_DICT)
		onion_dict_add(dict, key, value, OD_DUP_ALL|OD_DICT);
	else
		onion_dict_add(dict, key, value, OD_DUP_ALL);
}

/**
 * @short Creates a full duplicate of the dict
 * @memberof onion_dict_t
 * 
 * Its actually the same, but with refcount increased, so future frees will free the dict
 * only on latest one.
 * 
 * Any change on one dict is made also on the other one, as well as rwlock... This is usefull on a multhreaded
 * environment so that multiple threads cna have the same dict and free it when not in use anymore.
 */
onion_dict *onion_dict_hard_dup(onion_dict *dict){
	onion_dict *d=onion_dict_new();
	onion_dict_preorder(dict, onion_dict_hard_dup_helper, d);
	return d;
}


/// Removes a node and its data
static void onion_dict_node_free(onion_dict_node *node){
	if (node->left)
		onion_dict_node_free(node->left);
	if (node->right)
		onion_dict_node_free(node->right);

	onion_dict_node_data_free(&node->data);
	free(node);
}

/**
 * @short Removes the full dict struct from mem.
 * @memberof onion_dict_t
 */
void onion_dict_free(onion_dict *dict){
#ifdef HAVE_PTHREADS
	pthread_mutex_lock(&dict->refmutex);
#endif
	dict->refcount--;
	//ONION_DEBUG0("Free %p refcount %d", dict, dict->refcount);
	int remove=(dict->refcount==0);
#ifdef HAVE_PTHREADS
	pthread_mutex_unlock(&dict->refmutex);
#endif
	if(remove){
#ifdef HAVE_PTHREADS
		pthread_rwlock_destroy(&dict->lock);
		pthread_mutex_destroy(&dict->refmutex);
#endif
		if (dict->root)
			onion_dict_node_free(dict->root);
		free(dict);
	}
}
	
/**
 * @short Searchs for a given key, and returns that node and its parent (if parent!=NULL) 
 * @memberof onion_dict_t
 *
 * If not found, returns the parent where it should be. Nice for adding too.
 */
static const onion_dict_node *onion_dict_find_node(const onion_dict_node *current, const char *key, const onion_dict_node **parent){
	if (!current){
		return NULL;
	}
	signed char cmp=strcmp(key, current->data.key);
	//ONION_DEBUG0("%s cmp %s = %d",key, current->data.key, cmp);
	if (cmp==0)
		return current;
	if (parent) *parent=current;
	if (cmp<0)
		return onion_dict_find_node(current->left, key, parent);
	if (cmp>0)
		return onion_dict_find_node(current->right, key, parent);
	return NULL;
}

/**
 * @short Searchs for a given key, and returns that node and its parent (if parent!=NULL). Case insensitive version.
 * @memberof onion_dict_t
 *
 * If not found, returns the parent where it should be. Nice for adding too.
 */
static const onion_dict_node *onion_dict_ifind_node(const onion_dict_node *current, const char *key, const onion_dict_node **parent){
  if (!current){
    return NULL;
  }
  signed char cmp=strcasecmp(key, current->data.key);
  //ONION_DEBUG0("%s cmp %s = %d",key, current->data.key, cmp);
  if (cmp==0)
    return current;
  if (parent) *parent=current;
  if (cmp<0)
    return onion_dict_ifind_node(current->left, key, parent);
  if (cmp>0)
    return onion_dict_ifind_node(current->right, key, parent);
  return NULL;
}


/// Allocates a new node data, and sets the data itself.
static onion_dict_node *onion_dict_node_new(const char *key, const void *value, int flags){
	onion_dict_node *node=malloc(sizeof(onion_dict_node));

	onion_dict_set_node_data(&node->data, key, value, flags);
	
	node->left=NULL;
	node->right=NULL;
	node->level=1;
	return node;
}

/// Sets the data on the node, on the right way.
static void onion_dict_set_node_data(onion_dict_node_data *data, const char *key, const void *value, int flags){
	//ONION_DEBUG("Set data %02X",flags);
	if ((flags&OD_DUP_KEY)==OD_DUP_KEY) // not enought with flag, as its a multiple bit flag, with FREE included
		data->key=strdup(key);
	else
		data->key=key;
	if ((flags&OD_DUP_VALUE)==OD_DUP_VALUE){
		if (flags&OD_DICT)
			data->value=onion_dict_hard_dup((onion_dict*)value);
		else
			data->value=strdup(value);
	}
	else
		data->value=value;
	data->flags=flags;
}

/// Perform the skew operation
static onion_dict_node *skew(onion_dict_node *node){
	if (!node || !node->left || (node->left->level != node->level))
		return node;

	//ONION_DEBUG("Skew %p[%s]",node,node->data.key);
	onion_dict_node *t;
	t=node->left;
	node->left=t->right;
	t->right=node;
	return t;
}

/// Performs the split operation
static onion_dict_node *split(onion_dict_node *node){
	if (!node || !node->right || !node->right->right || (node->level != node->right->right->level))
		return node;
	
	//ONION_DEBUG("Split %p[%s]",node,node->data.key);
	onion_dict_node *t;
	t=node->right;
	node->right=t->left;
	t->left=node;
	t->level++;
	return t;
}

/// Decrease a level
static void decrease_level(onion_dict_node *node){
	int level_left=node->left ? node->left->level : 0;
	int level_right=node->right ? node->right->level : 0;
	int should_be=((level_left<level_right) ? level_left : level_right) + 1;
	if (should_be < node->level){
		//ONION_DEBUG("Decrease level %p[%s] level %d->%d",node, node->data.key, node->level, should_be);
		node->level=should_be;
		//ONION_DEBUG0("%p",node->right);
		if (node->right && ( should_be < node->right->level) ){
			//ONION_DEBUG("Decrease level right %p[%s], level %d->%d",node->right, node->right->data.key, node->right->level, should_be);
			node->right->level=should_be;
		}
	}
}

/**
 * @short AA tree insert
 * 
 * Returns the root node of the subtree
 */
static onion_dict_node  *onion_dict_node_add(onion_dict_node *node, onion_dict_node *nnode){
	if (node==NULL){
		//ONION_DEBUG("Add here %p",nnode);
		return nnode;
	}
	signed int cmp=strcmp(nnode->data.key, node->data.key);
	//ONION_DEBUG0("cmp %d, %X, %X %X",cmp, nnode->data.flags,nnode->data.flags&OD_REPLACE, OD_REPLACE);
	if ((cmp==0) && (nnode->data.flags&OD_REPLACE)){
		//ONION_DEBUG("Replace %s with %s", node->data.key, nnode->data.key);
		onion_dict_node_data_free(&node->data);
		memcpy(&node->data, &nnode->data, sizeof(onion_dict_node_data));
		free(nnode);
		return node;
	}
	else if (cmp<0){
		node->left=onion_dict_node_add(node->left, nnode);
		//ONION_DEBUG("%p[%s]->left=%p[%s]",node, node->data.key, node->left, node->left->data.key);
	}
	else{ // >=
		node->right=onion_dict_node_add(node->right, nnode);
		//ONION_DEBUG("%p[%s]->right=%p[%s]",node, node->data.key, node->right, node->right->data.key);
	}
	
	node=skew(node);
	node=split(node);
	
	return node;
}


/**
 * @memberof onion_dict_t
 * Adds a value in the tree.
 */
void onion_dict_add(onion_dict *dict, const char *key, const void *value, int flags){
	dict->root=onion_dict_node_add(dict->root, onion_dict_node_new(key, value, flags));
}

/// Frees the memory, if necesary of key and value
static void onion_dict_node_data_free(onion_dict_node_data *data){
	if (data->flags&OD_FREE_KEY){
		free((char*)data->key);
	}
	if (data->flags&OD_FREE_VALUE){
		if (data->flags&OD_DICT){
			onion_dict_free((onion_dict*)data->value);
		}
		else
			free((char*)data->value);
	}
}

/// AA tree remove the node
static onion_dict_node *onion_dict_node_remove(onion_dict_node *node, const char *key){
	if (!node)
		return NULL;
	int cmp=strcmp(key, node->data.key);
	if (cmp<0){
		node->left=onion_dict_node_remove(node->left, key);
		//ONION_DEBUG("%p[%s]->left=%p[%s]",node, node->data.key, node->left, node->left ? node->left->data.key : "NULL");
	}
	else if (cmp>0){
		node->right=onion_dict_node_remove(node->right, key);
		//ONION_DEBUG("%p[%s]->right=%p[%s]",node, node->data.key, node->right, node->right ? node->right->data.key : "NULL");
	}
	else{ // Real remove
		//ONION_DEBUG("Remove here %p", node);
		onion_dict_node_data_free(&node->data);
		if (node->left==NULL && node->right==NULL){
			free(node);
			return NULL;
		}
		if (node->left==NULL){
			onion_dict_node *t=node->right; // Get next key node
			while (t->left) t=t->left;
			//ONION_DEBUG("Set data from %p[%s] to %p[already deleted %s]",t,t->data.key, node, key);
			memcpy(&node->data, &t->data, sizeof(onion_dict_node_data));
			t->data.flags=0; // No double free later, please
			node->right=onion_dict_node_remove(node->right, t->data.key);
			//ONION_DEBUG("%p[%s]->right=%p[%s]",node, node->data.key, node->right, node->right ? node->right->data.key : "NULL");
		}
		else{
			onion_dict_node *t=node->left; // Get prev key node
			while (t->right) t=t->right;
			
			memcpy(&node->data, &t->data, sizeof(onion_dict_node_data));
			t->data.flags=0; // No double free later, please
			node->left=onion_dict_node_remove(node->left, t->data.key);
			//ONION_DEBUG("%p[%s]->left=%p[%s]",node, node->data.key, node->left, node->left ? node->left->data.key : "NULL");
		}
	}
	decrease_level(node);
	node=skew(node);
	if (node->right){
		node->right=skew(node->right);
		if (node->right->right)
			node->right->right=skew(node->right->right);
	}
	node=split(node);
	if (node->right)
		node->right=split(node->right);
	return node;
}


/**
 * @memberof onion_dict_t
 * Removes the given key. 
 *
 * Returns if it removed any node.
 */ 
int onion_dict_remove(onion_dict *dict, const char *key){
	dict->root=onion_dict_node_remove(dict->root, key);
	return 1;
}

/**
 * @short Gets a value. For dicts returns NULL; use onion_dict_get_dict.
 * @memberof onion_dict_t
 */
const char *onion_dict_get(const onion_dict *dict, const char *key){
	const onion_dict_node *r;
	r=onion_dict_find_node(dict->root, key, NULL);
	if (r && !(r->data.flags&OD_DICT))
		return r->data.value;
	return NULL;
}

/**
 * @short Gets a value Case-Insensitive version. For dicts returns NULL; use onion_dict_get_dict.
 * @memberof onion_dict_t
 */
const char *onion_dict_iget(const onion_dict *dict, const char *key){
  const onion_dict_node *r;
  r=onion_dict_ifind_node(dict->root, key, NULL);
  if (r && !(r->data.flags&OD_DICT))
    return r->data.value;
  return NULL;
}


/**
 * @short Gets a value, only if its a dict
 * @memberof onion_dict_t
 */
onion_dict *onion_dict_get_dict(const onion_dict *dict, const char *key){
	const onion_dict_node *r;
	r=onion_dict_find_node(dict->root, key, NULL);
	if (r){
		if (r->data.flags&OD_DICT)
			return (onion_dict*)r->data.value;
	}
	return NULL;
}


static void onion_dict_node_print_dot(const onion_dict_node *node){
	if (node->right){
		fprintf(stderr,"\"%s\" -> \"%s\" [label=\"R\"];\n",node->data.key, node->right->data.key);
		onion_dict_node_print_dot(node->right);
	}
	if (node->left){
		fprintf(stderr,"\"%s\" -> \"%s\" [label=\"L\"];\n",node->data.key, node->left->data.key);
		onion_dict_node_print_dot(node->left);
	}
}

/**
* @memberof onion_dict_t
  * Prints a graph on the form:
 *
 * key1 -> key0;
 * key1 -> key2;
 * ...
 *
 * User of this function has to write the 'digraph G{' and '}'
 */
void onion_dict_print_dot(const onion_dict *dict){
	if (dict->root)
		onion_dict_node_print_dot(dict->root);
}

static void onion_dict_node_preorder(const onion_dict_node *node, void *func, void *data){
	void (*f)(void *data, const char *key, const void *value, int flags);
	f=func;
	if (node->left)
		onion_dict_node_preorder(node->left, func, data);
	
	f(data, node->data.key, node->data.value, node->data.flags);
	
	if (node->right)
		onion_dict_node_preorder(node->right, func, data);
}

/**
 * @short Executes a function on each element, in preorder by key.
*  @memberof onion_dict_t
  * 
 * The function is of prototype void func(void *data, const char *key, const void *value, int flags);
 */
void onion_dict_preorder(const onion_dict *dict, void *func, void *data){
	if (!dict || !dict->root)
		return;
	onion_dict_node_preorder(dict->root, func, data);
}

static int onion_dict_node_count(const onion_dict_node *node){
	int c=1;
	if (node->left)
		c+=onion_dict_node_count(node->left);
	if (node->right)
		c+=onion_dict_node_count(node->right);
	return c;
}

/**
 * @short Counts elements
 * @memberof onion_dict_t
 */
int onion_dict_count(const onion_dict *dict){
	if (dict && dict->root)
		return onion_dict_node_count(dict->root);
	return 0;
}

/**
 * Do a read lock. Several can lock for reading, but only can be writing.
 * @memberof onion_dict_t
 */
void onion_dict_lock_read(const onion_dict *dict){
#ifdef HAVE_PTHREADS
	pthread_rwlock_rdlock((pthread_rwlock_t*)&dict->lock);
#endif
}

/**
 * @short Do a read lock. Several can lock for reading, but only can be writing.
 * @memberof onion_dict_t
 */
void onion_dict_lock_write(onion_dict *dict){
#ifdef HAVE_PTHREADS
	pthread_rwlock_wrlock(&dict->lock);
#endif
}

/**
 * @short Free latest lock be it read or write.
 * @memberof onion_dict_t
 */
void onion_dict_unlock(onion_dict *dict){
#ifdef HAVE_PTHREADS
	pthread_rwlock_unlock(&dict->lock);
#endif
}

/**
 * @short Helps to prepare each pair.
 */
static void onion_dict_json_preorder(onion_block *block, const char *key, const void *value, int flags){
	if (!onion_block_size(block)) // Error somewhere.
		return;
	char *s;
	s=onion_c_quote_new(key);
	if (s==NULL){
		onion_block_clear(block);
		return;
	}
	onion_block_add_str(block, s);
	free(s);
	onion_block_add_char(block, ':');
	if (flags&OD_DICT){
		onion_block *tmp;
		tmp=onion_dict_to_json((onion_dict*)value);
		if (!tmp){
			onion_block_clear(block);
			return;
		}
		onion_block_add_block(block, tmp);
		onion_block_free(tmp);
	}
	else{
		s=onion_c_quote_new(value);
		if (s==NULL){
			onion_block_clear(block);
			return;
		}
		onion_block_add_str(block, s);
		free(s);
	}
	onion_block_add_data(block, ", ",2);
}

/**
 * @short Converts a dict to a json string
 * @memberof onion_dict_t
 * 
 * Given a dictionary and a buffer (with size), it writes a json dictionary to it.
 * 
 * @returns an onion_block with the json data, or NULL on error
 */
block *onion_dict_to_json(onion_dict *dict){
	onion_block *block=onion_block_new();
	
	onion_block_add_char(block, '{');
	if (dict && dict->root)
		onion_dict_node_preorder(dict->root, (void*)onion_dict_json_preorder, block);


	int s=onion_block_size(block);
	if (s==0){ // Error.
		onion_block_clear(block);
		return NULL;
	}
	if (s!=1) // To remove a final ", "
		onion_block_rewind(block, 2);
	
	onion_block_add_char(block, '}');

	
	return block;
}

/**
 * @short Gets a dictionary string value, recursively
 * @memberof onion_dict_t
 * 
 * Loops inside given dictionaries to get the given value
 * 
 * @param dict The dictionary
 * @param key The key list, one per arg, end with NULL
 * @returns The const string if valid, or NULL
 */
const char *onion_dict_rget(const onion_dict *dict, const char *key, ...){
	const onion_dict *d=dict;
	const char *k=key;
	const char *nextk;
	va_list va;
	va_start(va, key);
	while (d){
		nextk=va_arg(va, const char *);
		if (!nextk){
			va_end(va);
			return onion_dict_get(d, k);
		}
		d=onion_dict_get_dict(d, k);
		k=nextk;
	}
	va_end(va);
	return NULL;
}


/**
 * @short Gets a dictionary dict value, recursively
 * @memberof onion_dict_t
 * 
 * Loops inside given dictionaries to get the given value
 * 
 * @param dict The dictionary
 * @param key The key list, one per arg, end with NULL
 * @returns The const string if valid, or NULL
 */
onion_dict *onion_dict_rget_dict(const onion_dict *dict, const char *key, ...){
	onion_dict *d=(onion_dict*)dict;
	const char *k=key;
	va_list va;
	va_start(va, key);
	while (d){
		d=onion_dict_get_dict(d, k);
		k=va_arg(va, const char *);
		if (!k){
			va_end(va);
			return d;
		}
	}
	va_end(va);
	return NULL;
}
