#ifdef SK_DBUG
#include <assert.h>
#endif
#include "skerror.h"
#include "sknode.h"
#include "skutils.h"
#include <stdlib.h>
#include <string.h>

/* Err msg buffer size */
#define ERR_SIZE 100

skJsonNode*
skJsonNode_default(skJsonNode* parent)
{
    skJsonNode* node;

    node = malloc(sizeof(skJsonNode));
    if(is_null(node)) {
        THROW_ERR(OutOfMemory);
        return NULL;
    }

    /* Leave data field as random garbo */
    node->parent = parent;
    node->index  = 0;

    return node;
}

skJsonNode*
skJsonObject_new(skJsonNode* parent)
{
    skJsonNode* object_node;

    object_node = skJsonNode_default(parent);
    if(is_null(object_node)) {
        return NULL;
    }

    object_node->data.j_object = skHashTable_new(
        NULL,
        (CmpKeyFn) strcmp,
        (FreeKeyFn) free,
        (FreeValueFn) skJsonNode_drop);

    if(is_null(object_node->data.j_object)) {
        return NULL;
    }

    object_node->type = SK_OBJECT_NODE;
    return object_node;
}

skJsonNode*
skJsonArray_new(skJsonNode* parent)
{
    skJsonNode* array_node;

    array_node = skJsonNode_default(parent);
    if(is_null(array_node)) {
        return NULL;
    }

    array_node->data.j_array = skVec_new(sizeof(skJsonNode));
    if(is_null(array_node->data.j_array)) {
        return NULL;
    }

    array_node->type = SK_ARRAY_NODE;
    return array_node;
}

skJsonNode*
skJsonError_new(skJsonString msg, skJsonState state, skJsonNode* parent)
{
    skJsonNode* node;
    char*       error;
    char        errmsg[ERR_SIZE];

    node = skJsonNode_default(parent);
    if(is_null(node)) {
        return NULL;
    }

    sprintf(
        errmsg,
        "%s: line %lu, col %lu, depth %lu\n",
        msg,
        state.ln,
        state.col,
        state.depth);

    error = strdup_ansi(errmsg);
    if(is_null(error)) {
        return NULL;
    }

    node->type       = SK_ERROR_NODE;
    node->data.j_err = error;

    return node;
}

skJsonNode*
skJsonString_new(skJsonString str, skJsonNode* parent)
{
    skJsonNode* node;

    node = skJsonNode_default(parent);
    if(is_null(node)) {
        return NULL;
    }

    node->type          = SK_STRING_NODE;
    node->data.j_string = str;

    return node;
}

skJsonNode*
skJsonInteger_new(skJsonInteger number, skJsonNode* parent)
{
    skJsonNode* node;

    node = skJsonNode_default(parent);
    if(is_null(node)) {
        return NULL;
    }

    node->type       = SK_INT_NODE;
    node->data.j_int = number;

    return node;
}

skJsonNode*
skJsonDouble_new(skJsonDouble number, skJsonNode* parent)
{
    skJsonNode* node;

    node = skJsonNode_default(parent);
    if(is_null(node)) {
        return NULL;
    }

    node->type          = SK_DOUBLE_NODE;
    node->data.j_double = number;

    return node;
}

skJsonNode*
skJsonBool_new(skJsonBool boolean, skJsonNode* parent)
{
    skJsonNode* node;

    node = skJsonNode_default(parent);
    if(is_null(node)) {
        return NULL;
    }

    node->type           = SK_BOOL_NODE;
    node->data.j_boolean = boolean;

    return node;
}

skJsonNode*
skJsonNull_new(skJsonNode* parent)
{
    skJsonNode* node;

    node = skJsonNode_default(parent);
    if(is_null(node)) {
        return NULL;
    }

    node->type = SK_NULL_NODE;

    return node;
}

void
skJsonNode_drop(skJsonNode* node)
{
    skJsonNode* parent;

    if(!is_null(node)) {
        switch(node->type) {
            case SK_OBJECT_NODE:
                skHashTable_drop(node->data.j_object);
                break;
            case SK_ARRAY_NODE:
                skVec_drop(node->data.j_array, (FreeFn) skJsonNode_drop);
                break;
            case SK_STRING_NODE:
                free(node->data.j_string);
                break;
            case SK_ERROR_NODE:
                free(node->data.j_err);
            default:
                break;
        }

        parent = node->parent;
        if(!is_null(parent) && parent->type == SK_ARRAY_NODE) {
            /* Json Array nodes are directly copied into the json array (vec)
             * this is why we can only remove these values instead of freeing.*/
            skVec_remove(parent->data.j_array, node->index, NULL);
        } else {
            /* All the other nodes that do not have a parent or are part of
             * the Json Object can be freed, note that the situation where
             * the node is part of the Json Object and is getting directly freed
             * is when we are dropping the whole Json Object, it is impossible
             * for user to directly free/drop the node that is part of the Json Object */
            free(node);
        }
    }
}
