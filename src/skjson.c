/* clang-format off */
#ifdef SK_DBUG
#include <assert.h>
#endif
#include "skerror.h"
#include "skparser.h" /* Make sure skparser.h which includes sknode.h is included before skjson.h */
#include "skjson.h"
#include "skutils.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Marker macro for private functions */
#define PRIVATE(ret)             static ret
/* Check if the node is valid (not null) and matches the type 't' */
#define valid_with_type(node, t) ((node) != NULL && (node)->type == (t))
/* Check if the node is 'valid_with_type' and has no parent */
#define can_be_inserted(node, type) \
    (valid_with_type((node), (type)) && (node)->parent_arena.ptr == NULL)
/* Check if node is of type of member and is part of the object */
#define is_member_in_object(json)                              \
    ((json)->type == SK_MEMBER_NODE && is_some((json)->parent) \
     && (json)->parent->type == SK_OBJECT_NODE)
/* Link the node with the parent arena */
#define link(node, parent)                                                    \
    do {                                                                      \
        (node)->parent_arena.ptr  = skVec_inner_unsafe(parent->data.j_array); \
        (node)->parent_arena.type = parent->type;                             \
    } while(0)
/* Unlinks the node from the parent */
#define unlink(node) (node)->parent_arena.ptr = NULL
/* Copies the link of 'dst' node into 'src' node. Then they will point to the
 * same parent_arena. */
#define copylink(src, dst) \
    (src)->parent_arena.ptr = (dst)->parent_arena.ptr; \
    (src)->parent_arena.type = (dst)->parent_arena.type

typedef struct _Serializer Serializer;

PRIVATE(bool) Serializer_serialize(Serializer* serializer, skJson* json);
PRIVATE(bool) Serializer_serialize_number(Serializer* serializer, skJson* json);
PRIVATE(bool) Serializer_serialize_string(Serializer* serializer, const char*);
PRIVATE(bool) Serializer_serialize_bool(Serializer* serializer, bool boolean);
PRIVATE(bool) Serializer_serialize_null(Serializer* serializer);
PRIVATE(bool) Serializer_serialize_array(Serializer* serializer, skVec* array);
PRIVATE(bool) Serializer_serialize_object(Serializer* serializer, skVec* table);
PRIVATE(void) drop_nonprim_elements(skJson* json);
PRIVATE(bool) array_push_node_checked(skJson* json, skJson* node);
PRIVATE(int) compare_tuples(const skObjTuple* a, const skObjTuple* b);

struct _Serializer {
    unsigned char* buffer;
    size_t         length;
    size_t         offset;
    size_t         depth;
    bool           expand;
    bool           user_provided;
};

/* Create a new Json Element from the given buffer 'buff' of parsable text
 * with length 'bufsize', return NULL in case 'buff' and 'bufsize' are NULL or 0,
 * or if Json Element construction failed, otherwise return the root Json Element. */
PUBLIC(skJson) skJson_parse(char* buff, size_t bufsize)
{
    skScanner* scanner;
    skJson json;
    bool oom;

    oom = false;
    json.type = SK_ERROR_NODE;

    if(is_null(buff) || bufsize == 0) {
        return json;
    }

    if(is_null(scanner = skScanner_new(buff, bufsize))) {
        return json;
    } else {
        skScanner_next(scanner); /* Fetch first token */
    }

    /* Construct the parse tree */
    json = skJsonNode_parse(scanner, NULL, &oom);
    /* We are done scanning */
    free(scanner);

    return json;
}

PUBLIC(const char*) skJson_error(const skJson* json)
{
    if(valid_with_type(json, SK_ERROR_NODE)) {
        return json->data.j_err;
    } else {
        THROW_ERR(WrongNodeType);
        return NULL;
    }
}

/* Drops the node preserving valid json state according to json standard. 
 * Reason for why array and object logic is separated even though it is exactly the same
 * is in case of future updates/rewrites where Json Object might be using different
 * data-structure other than vector internally. That way it will easier to maintain
 * this code, therefore the separate member of union called 'j_object' even though
 * currently it is the same data structure as 'j_array'. */
PUBLIC(void) skJson_drop(skJson* json)
{
    void* arena;
    skJson null_node;
    skJson* old;
    size_t  idx;

    if(is_null(json) || json->type == SK_DROPPED_NODE) {
        return;
    }

    if(is_some(arena = json->parent_arena.ptr)) {
        switch(json->parent_arena.type) {
            case SK_ARRAY_NODE:
                idx = ((char*) json - (char*) skVec_inner_unsafe((skVec*) arena)) 
                    / sizeof(skJson);

                null_node = RawNode_new(SK_NULL_NODE, NULL);
                copylink(&null_node, json);

                old = skVec_index((skVec*) arena, idx);
                skJsonNode_drop(old);
                *old = null_node;
                break;
            case SK_OBJECT_NODE:
                idx = ((char*) json - (char*) skVec_inner_unsafe((skVec*) arena))
                    / sizeof(skObjTuple);
#ifdef SK_DBUG
                assert(skVec_remove((skVec*) arena, idx, (FreeFn) skObjTuple_drop));
#else
                skVec_remove((skVec*) arena, idx, (FreeFn) skObjTuple_drop);
#endif
                break;
            default:
#ifdef SK_DBUG
                assert(false);
#endif
                THROW_ERR(UnreachableCode);
                break;
        }
    } else {
        skJsonNode_drop(json);
    }

    json->type = SK_DROPPED_NODE;
}

PUBLIC(int) skJson_type(const skJson* json)
{
    return (is_some(json)) ? (int) json->type : -1;
}

PUBLIC(bool) skJson_has_parent(const skJson* json)
{
    return (is_some(json) && is_some(json->parent_arena.ptr)) ? true : false;
}

PUBLIC(long int) skJson_integer_value(const skJson* json, int* cntrl)
{
    if(!valid_with_type(json, SK_INT_NODE)) {
        THROW_ERR(WrongNodeType);
        *cntrl = -1;
        return 0;
    }

    *cntrl = 0;
    return json->data.j_int;
}

PUBLIC(double) skJson_double_value(const skJson* json, int* cntrl)
{
    if(!valid_with_type(json, SK_DOUBLE_NODE)) {
        THROW_ERR(WrongNodeType);
        *cntrl = -1;
        return 0;
    }

    *cntrl = 0;
    return json->data.j_double;
}

PUBLIC(bool) skJson_bool_value(const skJson* json)
{
    if(!valid_with_type(json, SK_BOOL_NODE)) {
        THROW_ERR(WrongNodeType);
        return -1;
    }

    return json->data.j_boolean;
}

PUBLIC(char*) skJson_string_value(const skJson* json)
{
    if(!valid_with_type(json, SK_STRING_NODE) && json->type != SK_STRINGLIT_NODE) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    return strdup_ansi(json->data.j_string);
}

PRIVATE(void) drop_nonprim_elements(skJson* json)
{
    switch(json->type) {
        case SK_STRING_NODE:
            free(json->data.j_string);
            break;
        case SK_ARRAY_NODE:
            skVec_drop(json->data.j_array, (FreeFn) skJsonNode_drop);
            break;
        case SK_OBJECT_NODE:
            skVec_drop(json->data.j_object, (FreeFn) skObjTuple_drop);
            break;
        default:
            break;
    }
}

PUBLIC(skJson*) skJson_transform_into_int(skJson* json, long int n)
{
    if(is_null(json)) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    drop_nonprim_elements(json);

    json->data.j_int = n;
    json->type       = SK_INT_NODE;

    return json;
}


PUBLIC(skJson*) skJson_transform_into_double(skJson* json, double n)
{
    if(is_null(json)) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    drop_nonprim_elements(json);

    json->data.j_double = n;
    json->type          = SK_DOUBLE_NODE;

    return json;
}

PUBLIC(skJson*) skJson_transform_into_bool(skJson* json, bool boolean)
{
    if(is_null(json)) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    drop_nonprim_elements(json);

    json->data.j_boolean = boolean;
    json->type           = SK_BOOL_NODE;

    return json;
}

PUBLIC(skJson*) skJson_transform_into_stringlit(skJson* json, const char* string_ref)
{
    skStrSlice slice;

    if(is_null(json)) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    slice = skSlice_new(string_ref, strlen(string_ref) - 1);

    if(!skJsonString_isvalid(&slice)) {
        THROW_ERR(InvalidString);
        return NULL;
    }

    drop_nonprim_elements(json);

    json->data.j_string = discard_const(string_ref);
    json->type          = SK_STRINGLIT_NODE;

    return json;
}

PUBLIC(skJson*) skJson_transform_into_string(skJson* json, const char* string)
{
    skStrSlice slice;
    char*      new_str;

    if(is_null(json)) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    slice = skSlice_new(string, strlen(string) - 1);

    if(!skJsonString_isvalid(&slice)) {
        THROW_ERR(InvalidString);
        return NULL;
    }

    new_str = strdup_ansi(string);

    if(!is_null(new_str)) {
        return NULL;
    }

    drop_nonprim_elements(json);

    json->data.j_string = new_str;
    json->type          = SK_STRING_NODE;

    return json;
}

PUBLIC(skJson*) skJson_transform_into_empty_array(skJson* json)
{
    skVec* array;

    if(is_null(json)) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    array = skVec_new(sizeof(skJson));

    if(is_null(array)) {
        return NULL;
    }

    drop_nonprim_elements(json);

    json->data.j_array = array;
    json->type         = SK_ARRAY_NODE;

    return json;
}

PUBLIC(skJson*) skJson_transform_into_empty_object(skJson* json)
{
    skVec* table;

    if(is_null(json)) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    if(is_null(table = skVec_new(sizeof(skJson)))) {
        return NULL;
    }

    drop_nonprim_elements(json);

    json->data.j_object = table;
    json->type          = SK_OBJECT_NODE;

    return json;
}

PUBLIC(skJson) skJson_integer_new(long int n)
{
    return IntNode_new(n, NULL);
}

PUBLIC(bool) skJson_integer_set(skJson* json, long int n)
{
    if(!valid_with_type(json, SK_INT_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    json->data.j_int = n;
    return true;
}

PUBLIC(skJson) skJson_double_new(double n)
{
    return DoubleNode_new(n, NULL);
}

PUBLIC(bool) skJson_double_set(skJson* json, double n)
{
    if(!valid_with_type(json, SK_DOUBLE_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    json->data.j_double = n;
    return true;
}

PUBLIC(skJson) skJson_bool_new(bool boolean)
{
    return BoolNode_new(boolean, NULL);
}

PUBLIC(bool) skJson_bool_set(skJson* json, bool boolean)
{
    if(!valid_with_type(json, SK_BOOL_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    json->data.j_boolean = boolean;
    return true;
}

PUBLIC(skJson) skJson_null_new(void)
{
    return RawNode_new(SK_NULL_NODE, NULL);
}

PRIVATE(skJson) skJson_string_new_internal(const char* string, skNodeType type, skJson* parent)
{
    skStrSlice slice;
    skJson string_node;

    /* Guilty until proven innocent */
    string_node.data.j_string = NULL;
    string_node.type = SK_ERROR_NODE;

    slice = skSlice_new(string, strlen(string) - 1);

    if(!skJsonString_isvalid(&slice)) {
        THROW_ERR(InvalidString);
        return string_node;
    }

    return StringNode_new(discard_const(string), type, parent);
}

PUBLIC(skJson) skJson_string_new(const char* string)
{
    return skJson_string_new_internal(string, SK_STRING_NODE, NULL);
}

PUBLIC(bool) skJson_string_set(skJson* json, const char* string)
{
    char*      new_str;
    skStrSlice slice;

    if(!valid_with_type(json, SK_STRING_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    slice = skSlice_new(string, strlen(string) - 1);

    if(!skJsonString_isvalid(&slice)) {
        THROW_ERR(InvalidString);
        return false;
    }

    if((new_str = strdup_ansi(string)) == NULL) {
        return false;
    }

    /* Drop old string and set the new one */
    free(json->data.j_string);
    json->data.j_string = new_str;

    return true;
}

PUBLIC(skJson) skJson_stringlit_new(const char* string)
{
    return skJson_string_new_internal(string, SK_STRINGLIT_NODE, NULL);
}

PUBLIC(bool) skJson_stringlit_set(skJson* json, const char* string)
{
    skStrSlice slice;

    if(!valid_with_type(json, SK_STRINGLIT_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    slice = skSlice_new(string, strlen(string) - 1);

    if(!skJsonString_isvalid(&slice)) {
        THROW_ERR(InvalidString);
        return false;
    }

    json->data.j_string = discard_const(string);
    return true;
}

PUBLIC(skJson) skJson_array_new(void)
{
    return ArrayNode_new(NULL);
}

PUBLIC(bool) skJson_array_push_str(skJson* json, const char* string)
{
    skJson string_node;

    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    string_node = skJson_string_new_internal(string, SK_STRING_NODE, json);

    if(string_node.type == SK_ERROR_NODE) {
        return false;
    }

    return skVec_push(json->data.j_array, &string_node);
}

PUBLIC(bool) skJson_array_insert_str(skJson* json, const char* string, size_t index)
{
    skJson string_node;

    if(!valid_with_type(json, SK_ARRAY_NODE) || is_null(string)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    string_node = skJson_string_new_internal(string, SK_STRING_NODE, json);

    if(string_node.type == SK_ERROR_NODE) {
        return false;
    }

    return skVec_insert(json->data.j_array, &string_node, index);
}

PUBLIC(bool) skJson_array_push_strlit(skJson* json, const char* string)
{
    skJson string_node;

    if(!valid_with_type(json, SK_ARRAY_NODE) || is_null(string)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    string_node = skJson_string_new_internal(string, SK_STRINGLIT_NODE, json);

    if(string_node.type == SK_ERROR_NODE) {
        return false;
    }

    return skVec_push(json->data.j_array, &string_node);
}

PUBLIC(bool) skJson_array_insert_strlit(skJson* json, const char* string, size_t index)
{
    skJson string_node;

    if(!valid_with_type(json, SK_ARRAY_NODE) || is_null(string)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    string_node = skJson_string_new_internal(string, SK_STRINGLIT_NODE, json);

    if(string_node.type == SK_ERROR_NODE) {
        return false;
    }

    return skVec_insert(json->data.j_array, &string_node, index);
}

PUBLIC(bool) skJson_array_push_int(skJson* json, long int n)
{
    skJson int_node;

    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    int_node = IntNode_new(n, json);
    return skVec_push(json->data.j_array, &int_node);
}

PUBLIC(bool) skJson_array_insert_int(skJson* json, long int n, size_t index)
{
    skJson int_node;

    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    int_node = IntNode_new(n, json);
    return skVec_insert(json->data.j_array, &int_node, index);
}

PUBLIC(bool) skJson_array_push_double(skJson* json, double n)
{
    skJson double_node;

    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    double_node = DoubleNode_new(n, json);
    return skVec_push(json->data.j_array, &double_node);
}

PUBLIC(bool) skJson_array_insert_double(skJson* json, double n, size_t index)
{
    skJson double_node;

    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    double_node = DoubleNode_new(n, json);
    return skVec_insert(json->data.j_array, &double_node, index);
}

PUBLIC(bool) skJson_array_push_bool(skJson* json, bool boolean)
{
    skJson bool_node;

    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    bool_node = BoolNode_new(boolean, json);
    return skVec_push(json->data.j_array, &bool_node);
}

PUBLIC(bool) skJson_array_insert_bool(skJson* json, bool boolean, size_t index)
{
    skJson bool_node;

    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    bool_node = BoolNode_new(boolean, json);
    return skVec_insert(json->data.j_array, &bool_node, index);
}

PUBLIC(bool) skJson_array_push_null(skJson* json)
{
    skJson null_node;

    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    null_node = RawNode_new(SK_NULL_NODE, json);
    return skVec_push(json->data.j_array, &null_node);
}

PUBLIC(bool) skJson_array_insert_null(skJson* json, size_t index)
{
    skJson null_node;

    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    null_node = RawNode_new(SK_NULL_NODE, json);
    return skVec_insert(json->data.j_array, &null_node, index);
}

PUBLIC(bool) skJson_array_push_element(skJson* json, skJson** element)
{
    if(!valid_with_type(json, SK_ARRAY_NODE) 
            || (*element)->type == SK_MEMBER_NODE 
            || is_some((*element)->parent_arena.ptr))
    {
        THROW_ERR(WrongNodeType);
        return false;
    } else {
        link(*element, json);
    }

    if(!skVec_push(json->data.j_array, *element)) {
        unlink(*element);
        return false;
    } else {
        free(*element);
        *element = NULL;
    }

    return false;
}

PUBLIC(bool) skJson_array_insert_element(skJson* json, skJson** element, size_t index)
{
    if(!valid_with_type(json, SK_ARRAY_NODE) 
            || (*element)->type == SK_MEMBER_NODE
            || is_some((*element)->parent_arena.ptr)) 
    {
        THROW_ERR(WrongNodeType);
        return false;
    } else {
        link(*element, json);
    }

    if(!skVec_insert(json->data.j_array, *element, index)) {
        unlink(*element);
        return true;
    } else {
        free(*element);
        *element = NULL;
    }

    return false;
}

PUBLIC(skJson) skJson_array_from_strings(const char* const* strings, size_t count)
{
    skJson arr_node;

    if((arr_node = ArrayNode_new(NULL)).type == SK_ERROR_NODE) {
        return arr_node;
    }

    for(; count--; strings++) {
        if(!skJson_array_push_str(&arr_node, discard_const(*strings))) {
            skJsonNode_drop(&arr_node);
            arr_node.data.j_array = NULL;
            arr_node.type = SK_ERROR_NODE;
            return arr_node;
        }
    }

    return arr_node;
}

PUBLIC(skJson) skJson_array_from_refs(const char* const* strings, size_t count)
{
    skJson arr_node;

    if((arr_node = ArrayNode_new(NULL)).type == SK_ERROR_NODE) {
        return arr_node;
    }

    while(count--) {
        if(!skJson_array_push_strlit(&arr_node, *strings++)) {
            skJsonNode_drop(&arr_node);
            arr_node.data.j_array = NULL;
            arr_node.type = SK_ERROR_NODE;
            return arr_node;
        }
    }

    return arr_node;
}

PUBLIC(skJson) skJson_array_from_integers(const int* integers, size_t count)
{
    skJson arr_node;

    if((arr_node = ArrayNode_new(NULL)).type == SK_ERROR_NODE) {
        return arr_node;
    }

    while(count--) {
        if(!skJson_array_push_int(&arr_node, *integers)) {
            skJsonNode_drop(&arr_node);
            arr_node.data.j_array = NULL;
            arr_node.type = SK_ERROR_NODE;
            return arr_node;
        }
    }

    return arr_node;
}

PUBLIC(skJson) skJson_array_from_doubles(const double* doubles, size_t count)
{
    skJson arr_node;

    if((arr_node = ArrayNode_new(NULL)).type == SK_ERROR_NODE) {
        return arr_node;
    }

    while(count--) {
        if(!skJson_array_push_int(&arr_node, *doubles++)) {
            skJsonNode_drop(&arr_node);
            arr_node.data.j_array = NULL;
            arr_node.type = SK_ERROR_NODE;
            return arr_node;
        }
    }

    return arr_node;
}

PUBLIC(skJson) skJson_array_from_booleans(const bool* booleans, size_t count)
{
    skJson arr_node;

    if((arr_node = ArrayNode_new(NULL)).type == SK_ERROR_NODE) {
        return arr_node;
    }

    while(count--) {
        if(!skJson_array_push_int(&arr_node, *booleans++)) {
            skJsonNode_drop(&arr_node);
            arr_node.data.j_array = NULL;
            arr_node.type = SK_ERROR_NODE;
            return arr_node;
        }
    }

    return arr_node;
}

PUBLIC(skJson) skJson_array_from_nulls(size_t count)
{
    skJson arr_node;

    if((arr_node = ArrayNode_new(NULL)).type == SK_ERROR_NODE) {
        return arr_node;
    }

    while(count--) {
        if(!skJson_array_push_null(&arr_node)) {
            skJsonNode_drop(&arr_node);
            arr_node.data.j_array = NULL;
            arr_node.type = SK_ERROR_NODE;
            return arr_node;
        }
    }

    return arr_node;
}

PUBLIC(skJson) skJson_array_from_elements(const skJson* const* elements, size_t count)
{
    skJson arr_node;

    if((arr_node = ArrayNode_new(NULL)).type == SK_ERROR_NODE) {
        return arr_node;
    }

    while(count--) {
        if(is_null(*elements)
           || !array_push_node_checked(&arr_node, discard_const(*elements++)))
        {
            skJsonNode_drop(&arr_node);
            arr_node.data.j_array = NULL;
            arr_node.type = SK_ERROR_NODE;
            return arr_node;
        }
    }

    return arr_node;
}

PRIVATE(bool) array_push_node_checked(skJson* json, skJson* node)
{
    link(node, json);
    if(!skVec_push(json->data.j_array, node)) {
        unlink(node);
        return false;
    }

    return true;
}

PUBLIC(skJson) skJson_array_pop(skJson* json)
{
    skJson node;
    skJson* temp;
    node.type = SK_ERROR_NODE;

    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return node;
    }

    if(is_some(temp = skVec_pop(json->data.j_array))) {
        unlink(temp);
        node = *temp;
    }

    return node;
}

PUBLIC(bool) skJson_array_remove(skJson* json, size_t index)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    return skVec_remove(json->data.j_array, index, (FreeFn) skJsonNode_drop);
}

PUBLIC(size_t) skJson_array_len(skJson* json)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return 0;
    }

    return skVec_len(json->data.j_array);
}

PUBLIC(skJson*) skJson_array_front(skJson* json)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    return skVec_front(json->data.j_array);
}

PUBLIC(skJson*) skJson_array_back(skJson* json)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    return skVec_back(json->data.j_array);
}

PUBLIC(skJson*) skJson_array_index(skJson* json, size_t index)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    return skVec_index(json->data.j_array, index);
}

PUBLIC(void) skJson_array_clear(skJson* json)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
        THROW_ERR(WrongNodeType);
        return;
    }

    skVec_clear(json->data.j_array, (FreeFn) skJsonNode_drop);
}

PUBLIC(skJson) skJson_object_new(void)
{
    return ObjectNode_new(NULL);
}

PUBLIC(bool) skJson_object_sort_ascending(skJson* json)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    return skVec_sort(json->data.j_object, (CmpFn) compare_tuples);
}

PRIVATE(int) compare_tuples(const skObjTuple* a, const skObjTuple* b)
{
    return strcmp(a->key, b->key);
}

PUBLIC(bool)
skJson_object_insert_element(
    skJson*         json,
    const char*     key,
    skJson*         element,
    size_t          index)
{
    skObjTuple tuple;

    if(!valid_with_type(json, SK_OBJECT_NODE)
            || is_some(element->parent_arena.ptr)) 
    {
        THROW_ERR(WrongNodeType);
        return false;
    }

    link(element, json);
    tuple.key = discard_const(key);
    tuple.value = *element;

    if(!skVec_insert(json->data.j_object, &tuple, index)) {
        unlink(element);
        return false;
    } else {
        element->type = SK_ERROR_NODE;
    }

    return true;
}

PUBLIC(bool) skJson_object_remove(skJson* json, size_t index)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    return skVec_remove(json->data.j_object, index, (FreeFn) skJsonNode_drop);
}

PUBLIC(bool)
skJson_object_remove_by_key(
    skJson*     json,
    const char* key,
    bool        sorted)
{
    skObjTuple dummy_tuple;

    if(!valid_with_type(json, SK_OBJECT_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    dummy_tuple.key = discard_const(key);

    return skVec_remove_by_key(
        json->data.j_object,
        &dummy_tuple,
        (CmpFn) compare_tuples,
        (FreeFn) skJsonNode_drop,
        sorted);
}

PUBLIC(skObjTuple*) skJson_object_index(const skJson* json, size_t index)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    return skVec_index(json->data.j_object, index);
}

PUBLIC(skObjTuple*)
skJson_object_index_by_key(
    const skJson* json,
    const char*       key,
    bool              sorted)
{
    skObjTuple dummy_tuple;

    if(!valid_with_type(json, SK_OBJECT_NODE)) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    dummy_tuple.key = discard_const(key);

    return skVec_get_by_key(
        json->data.j_object,
        &dummy_tuple,
        (CmpFn) compare_tuples,
        sorted);
}

PUBLIC(skJson*) skJson_objtuple_value(const skObjTuple* tuple)
{
    if(is_null(tuple)) {
        return NULL;
    }

    return discard_const(&tuple->value);
}

PUBLIC(char*) skJson_objtuple_key(const skObjTuple* tuple)
{
    if(is_null(tuple)) {
        return NULL;
    }

    return strdup_ansi(tuple->key);
}

PUBLIC(size_t) skJson_object_len(const skJson* json)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
        THROW_ERR(WrongNodeType);
        return 0;
    }

    return skVec_len(json->data.j_object);
}

PUBLIC(bool)
skJson_object_contains(const skJson* json, const char* key, bool sorted)
{
    skObjTuple dummy_tuple;

    if(!valid_with_type(json, SK_OBJECT_NODE)) {
        THROW_ERR(WrongNodeType);
        return false;
    }

    dummy_tuple.key = discard_const(key);

    return skVec_contains(
        json->data.j_object,
        &dummy_tuple,
        (CmpFn) compare_tuples,
        sorted);
}

PUBLIC(void) skJson_object_clear(skJson* json)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
        THROW_ERR(WrongNodeType);
        return;
    }

    skVec_clear(json->data.j_object, (FreeFn) skJsonNode_drop);
}

PRIVATE(Serializer) Serializer_new(size_t bufsize, bool expand)
{
    Serializer     pbuf;
    unsigned char* buffer;

    memset(&pbuf, 0, sizeof(Serializer));

    if(bufsize == 0) {
        bufsize = BUFSIZ;
    }

    if(is_null(buffer = malloc(bufsize))) {
        THROW_ERR(OutOfMemory);
        return pbuf;
    }

    pbuf.length = bufsize;
    pbuf.buffer = buffer;
    pbuf.expand = expand;

    return pbuf;
}

PRIVATE(Serializer) Serializer_from(unsigned char* buffer, size_t bufsize, bool expand)
{
    Serializer pbuf;

    memset(&pbuf, 0, sizeof(Serializer));
#ifdef SK_DBUG
    if(is_null(buffer) || bufsize == 0) {
        return pbuf;
    }
#endif
    pbuf.length        = bufsize;
    pbuf.buffer        = buffer;
    pbuf.expand        = expand;
    pbuf.user_provided = true;

    return pbuf;
}

PRIVATE(void) Serializer_drop(Serializer* serializer)
{
#ifdef SK_DBUG
    assert(is_some(serializer));
    assert(is_some(serializer->buffer));
#endif
    free(serializer->buffer);
    memset(serializer, 0, sizeof(Serializer));
}

PRIVATE(unsigned char*) Serializer_buffer_ensure(Serializer* serializer, size_t needed)
{
    unsigned char* newbuf;
    size_t         newsize;

#ifdef SK_DBUG
    assert(is_some(serializer));
    assert(is_some(serializer->buffer));
    assert(!(serializer->length > 0 && serializer->offset >= serializer->length));
#endif

    if(needed > INT_MAX) {
        THROW_ERR(AllocationTooLarge);
        if(!serializer->user_provided) {
            Serializer_drop(serializer);
        }
        return NULL;
    }

    needed += serializer->offset + 1; /* 1 for null terminator */

    /* If we got enough space return current position */
    if(needed <= serializer->length) {
        return serializer->buffer + serializer->offset;
    }

    /* Check if we are allowed to expand */
    if(!serializer->expand) {
        if(!serializer->user_provided) {
            Serializer_drop(serializer);
        }
        return NULL;
    }

    /* Else try expand the buffer */
    if(needed > (INT_MAX / 2)) {
        if(needed <= INT_MAX) {
            newsize = INT_MAX;
        } else {
            THROW_ERR(AllocationTooLarge);
            if(!serializer->user_provided) {
                Serializer_drop(serializer);
            }
            return NULL;
        }
    } else {
        newsize = needed * 2;
    }

    /* Try reallocate the buffer to 'newsize' */
    newbuf = realloc(serializer->buffer, newsize);

    if(is_null(newbuf)) {
        THROW_ERR(OutOfMemory);
        if(!serializer->user_provided) {
            Serializer_drop(serializer);
        }
        return NULL;
    }

    serializer->buffer = newbuf;
    serializer->length = newsize;

    return newbuf + serializer->offset;
}

PRIVATE(void) Serializer_offset_update(Serializer* serializer)
{
#ifdef SK_DBUG
    assert(is_some(serializer));
    assert(is_some(serializer->buffer));
#endif
    printf("Before updating offset -> %ld\n", serializer->offset);
    serializer->offset += strlen((char*) serializer->buffer + serializer->offset);
    printf("After updating offset -> %ld\n", serializer->offset);
}

PUBLIC(unsigned char*)
skJson_serialize_with_buffer(skJson* json, unsigned char* buffer, size_t size, bool expand)
{
    Serializer serializer;

    if(is_null(json) || is_null(buffer) || size == 0) {
        return NULL;
    }

    if(json->type == SK_ERROR_NODE) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    serializer = Serializer_from(buffer, size, expand);

#ifdef SK_DBUG
    assert(is_some(serializer.buffer));
#endif

    if(!Serializer_serialize(&serializer, json)) {
        return NULL;
    }

    return serializer.buffer;
}

PUBLIC(unsigned char*)
skJson_serialize_with_bufsize(skJson* json, size_t size, bool expand)
{
    Serializer serializer;

    if(is_null(json) || size == 0) {
        return NULL;
    }

    if(json->type == SK_ERROR_NODE) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    if(is_null((serializer = Serializer_new(size, expand)).buffer)) {
        return NULL;
    }

    if(!Serializer_serialize(&serializer, json)) {
        return NULL;
    }

    return serializer.buffer;
}

PUBLIC(unsigned char*) skJson_serialize(skJson* json)
{
    Serializer serializer;

    if(is_null(json)) {
        return NULL;
    }

    if(json->type == SK_ERROR_NODE) {
        THROW_ERR(WrongNodeType);
        return NULL;
    }

    if(is_null((serializer = Serializer_new(0, true)).buffer)) {
        return NULL;
    }

    if(!Serializer_serialize(&serializer, json)) {
        return NULL;
    }

    printf("Total len in bytes -> %ld\n", strlen((char*) serializer.buffer));
    return serializer.buffer;
}

PRIVATE(bool) Serializer_serialize(Serializer* serializer, skJson* json)
{
#ifdef SK_DBUG
    assert(is_some(json));
    assert(is_some(serializer));
    assert(is_some(serializer->buffer));
#endif
    switch(json->type) {
        case SK_STRING_NODE:
        case SK_STRINGLIT_NODE:
            return Serializer_serialize_string(serializer, json->data.j_string);
        case SK_INT_NODE:
        case SK_DOUBLE_NODE:
            return Serializer_serialize_number(serializer, json);
        case SK_BOOL_NODE:
            return Serializer_serialize_bool(serializer, json->data.j_boolean);
        case SK_NULL_NODE:
            return Serializer_serialize_null(serializer);
        case SK_ARRAY_NODE:
            return Serializer_serialize_array(serializer, json->data.j_array);
        case SK_OBJECT_NODE:
            printf("Serializing object\n");
            return Serializer_serialize_object(serializer, json->data.j_object);
        case SK_ERROR_NODE:
        default:
            THROW_ERR(SerializerInvalidJson);
            return false;
    }
}

PRIVATE(bool) Serializer_serialize_number(Serializer* serializer, skJson* json)
{
    char           buff[30];
    unsigned char* out;
    long int       len;

#ifdef SK_DBUG
    assert(is_some(serializer));
    assert(is_some(serializer->buffer));
    assert(is_some(json));
#endif

    if(json->type == SK_DOUBLE_NODE) {
        len = sprintf(buff, "%fl", json->data.j_double);
    } else {
        len = sprintf(buff, "%ld", json->data.j_int);
    }

    if(len < 0 || len >= 30) {
        THROW_ERR(SerializerNumberError);
        return false;
    }

    out = Serializer_buffer_ensure(serializer, len + sizeof("")); /* len + '\0' */

    if(is_null(out)) {
        return false;
    }

    strcat((char*) out, buff);
    Serializer_offset_update(serializer);

    return true;
}

PRIVATE(bool) Serializer_serialize_string(Serializer* serializer, const char* str)
{
    unsigned char* out;
    size_t         len;

#ifdef SK_DBUG
    assert(is_some(serializer));
    assert(is_some(serializer->buffer));
#endif

    len = strlen(str) + sizeof("\"\"");
    out = Serializer_buffer_ensure(serializer, 1 + len + sizeof("\""));

    if(is_null(out)) {
        return false;
    }

    out[0] = '\"';
    memcpy(out + 1, str, len);
    out[len + 1] = '\"';
    out[len + 2] = '\0';

    printf("Offset is %ld\n", serializer->offset);
    Serializer_offset_update(serializer);

    return true;
}

PRIVATE(bool) Serializer_serialize_bool(Serializer* serializer, bool boolean)
{
    unsigned char* out;

#ifdef SK_DBUG
    assert(is_some(serializer));
    assert(is_some(serializer->buffer));
#endif

    if(boolean) {
        if(is_null(out = Serializer_buffer_ensure(serializer, 5))) {
            return false;
        }
        strcpy((char*) out, "true");
    } else {
        if(is_null(out = Serializer_buffer_ensure(serializer, 6))) {
            return false;
        }
        strcpy((char*) out, "false");
    }

    Serializer_offset_update(serializer);
    printf("%s\n", serializer->buffer);
#ifdef SK_DBUG
    assert(
        strcmp((char*) serializer->buffer, "{\"verifiable_password_authentication\": false")
        == 0);
#endif
    return true;
}

PRIVATE(bool) Serializer_serialize_null(Serializer* serializer)
{
    unsigned char* out;

#ifdef SK_DBUG
    assert(is_some(serializer));
    assert(is_some(serializer->buffer));
#endif

    if(is_null(out = Serializer_buffer_ensure(serializer, 5))) {
        return false;
    }
    strcpy((char*) out, "null");
    Serializer_offset_update(serializer);
    return true;
}

PRIVATE(bool) Serializer_serialize_array(Serializer* serializer, skVec* vec)
{
    unsigned char* out;
    size_t         len, i;
    skJson*    current;

#ifdef SK_DBUG
    assert(is_some(serializer));
    assert(is_some(serializer->buffer));
#endif

    if(is_null(out = Serializer_buffer_ensure(serializer, 1))) {
        return false;
    }

    out[0] = '[';
    serializer->offset++;
    serializer->depth++;

    len = skVec_len(vec);

    for(i = 0; i < len; i++) {
        current = skVec_index(vec, i);
#ifdef SK_DBUG
        assert(is_some(current));
#endif
        if(!Serializer_serialize(serializer, current)) {
            return false;
        }

        if(i + 1 < len) {
            if(is_null(out = Serializer_buffer_ensure(serializer, 2))) {
                return false;
            }
            *out++             = ',';
            *out               = '\0';
            serializer->offset += 2;
        }
    }

    if(is_null(out = Serializer_buffer_ensure(serializer, 2))) {
        return false;
    }

    *out++ = ']';
    *out   = '\0';
    serializer->depth--;
    serializer->offset += 2;

    return true;
}

PRIVATE(bool) Serializer_serialize_object(Serializer* serializer, skVec* table)
{
    unsigned char* out;
    skObjTuple*    tuple;
    size_t         len, i;

#ifdef SK_DBUG
    assert(is_some(serializer));
    assert(is_some(serializer->buffer));
    assert(is_some(table));
#endif

    if(is_null(out = Serializer_buffer_ensure(serializer, 1))) {
        return false;
    }

    out[0] = '{';
    serializer->offset++;
    serializer->depth++;

    len = skVec_len(table);
    printf("table len is -> %ld\n", len);
    for(i = 0; i < len; i++) {
        tuple = skVec_index(table, i);
#ifdef SK_DBUG
        assert(is_some(tuple));
#endif
        if(!Serializer_serialize_string(serializer, tuple->key)) {
            return false;
        }

        if(is_null(out = Serializer_buffer_ensure(serializer, 2))) {
            return false;
        }

        *out               = ':';
        serializer->offset += 2;

        if(!Serializer_serialize(serializer, &tuple->value)) {
            return false;
        }

        if(is_null(out = Serializer_buffer_ensure(serializer, 2))) {
            return false;
        }

        if(i + 1 != len) {
            *out++             = ',';
            *out               = '\0';
            serializer->offset += 2;
        }
    }

    *out++             = '}';
    *out               = '\0';
    serializer->offset += 2;
    serializer->depth--;

    return true;
}
