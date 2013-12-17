
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _RBTREE_H_INCLUDED_
#define _RBTREE_H_INCLUDED_

typedef uintptr_t      rbtree_key_t;
typedef intptr_t       rbtree_key_int_t;

typedef struct rbtree_node_s  rbtree_node_t;

struct rbtree_node_s {
    rbtree_key_t       key;
    rbtree_node_t     *left;
    rbtree_node_t     *right;
    rbtree_node_t     *parent;
    char               color;
    char               data;
};


typedef struct rbtree_s  rbtree_t;


struct rbtree_s {
    rbtree_node_t     *root;
    rbtree_node_t     *sentinel;
};


#define rbtree_init(tree, s)                                           \
    rbtree_sentinel_init(s);                                              \
    (tree)->root = s;                                                         \
    (tree)->sentinel = s; 


void rbtree_insert(rbtree_t *tree,
    rbtree_node_t *node);
void rbtree_delete(rbtree_t *tree,
    rbtree_node_t *node);


#define rbt_red(node)               ((node)->color = 1)
#define rbt_black(node)             ((node)->color = 0)
#define rbt_is_red(node)            ((node)->color)
#define rbt_is_black(node)          (!rbt_is_red(node))
#define rbt_copy_color(n1, n2)      (n1->color = n2->color)


/* a sentinel must be black */

#define rbtree_sentinel_init(node)  rbt_black(node)


static inline rbtree_node_t *
rbtree_min(rbtree_node_t *node, rbtree_node_t *sentinel)
{
    while (node->left != sentinel) {
        node = node->left;
    }

    return node;
}


#endif 
