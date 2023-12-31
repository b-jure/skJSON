/* clang-format off */
#ifdef SK_DBUG
#include <assert.h>
#endif
#include "skerror.h"
#include "skparser.h" /* Make sure skparser.h which includes sknode.h is included before skjson.h */
#include "skjson.h"
#include "skutils.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* clang-format on */

/* Marker macro for private functions */
#define PRIVATE(ret)             static ret
/* Check if the node is valid (not null) and matches the type 't' */
#define valid_with_type(node, t) ((node) != NULL && (node)->type == (t))
/* Check if 'node' has a parent. */
#define has_parent(node)         (is_some((node)->parent_arena.ptr))
/* Link the node with the parent arena */
#define link_parent(node, parent)                                             \
    do {                                                                      \
        (node)->parent_arena.ptr  = skVec_inner_unsafe(parent->data.j_array); \
        (node)->parent_arena.type = parent->type;                             \
    } while(0)
/* Unlinks the node from the parent */
#define unlink_parent(node)           \
    (node)->parent_arena.ptr  = NULL; \
    (node)->parent_arena.type = SK_NONE_NODE
/* Copies the link of 'dst' node into 'src' node. Then they will point to the
 * same parent_arena. */
#define copylink(src, dst)                              \
    (src)->parent_arena.ptr  = (dst)->parent_arena.ptr; \
    (src)->parent_arena.type = (dst)->parent_arena.type
/* Checks if node is SK_NONE_NODE or SK_ERROR_NODE */
#define err_or_none(node) ((node)->type & (SK_NONE_NODE | SK_ERROR_NODE))

/**
 * Private typedefs
 *
 *
 * Json Serializer */
typedef struct _Serializer Serializer;
/* Generic CString push function for references and strings.
 * Used when constructing json array from array of strings/references. */
typedef skJsonBool (*CStrPush)(skJson*, const char*);

/* clang-format off */

/* Internal functions */
PRIVATE(void) drop_nonprim_elements(skJson* json);
PRIVATE(skJson) skJson_string_new_internal(const char* string, skNodeType type, skJson* parent);
PRIVATE(skJson) skJson_constructor_internal(void* val, skNodeType type, skJson* parent);
PRIVATE(skJsonBool) skJson_array_insert_internal(skJson* parent, const void* val, skNodeType type, size_t index, skJsonBool push, skJsonBool element);
PRIVATE(skJsonBool) array_push_node_checked(skJson* json, skJson* node);
PRIVATE(int) compare_tuples(const skObjTuple* a, const skObjTuple* b);
PRIVATE(skJsonBool) skJson_object_insert_internal(skJson* parent, const char* key, const void* val, skNodeType type, size_t index, skJsonBool push, skJsonBool element);
PRIVATE(Serializer) Serializer_new(size_t bufsize, skJsonBool expand);
PRIVATE(Serializer) Serializer_from(unsigned char* buffer, size_t bufsize, skJsonBool expand);
PRIVATE(void) Serializer_drop(Serializer* serializer);
PRIVATE(unsigned char*) Serializer_buffer_ensure(Serializer* serializer, size_t needed);
PRIVATE(void) Serializer_offset_update(Serializer* serializer);
PRIVATE(skJsonBool) Serializer_serialize(Serializer* serializer, skJson* json);
PRIVATE(skJsonBool) Serializer_serialize_number(Serializer* serializer, skJson* json);
PRIVATE(skJsonBool) Serializer_serialize_string(Serializer* serializer, const char*);
PRIVATE(skJsonBool) Serializer_serialize_bool(Serializer* serializer, skJsonBool boolean);
PRIVATE(skJsonBool) Serializer_serialize_null(Serializer* serializer);
PRIVATE(skJsonBool) Serializer_serialize_array(Serializer* serializer, skVec* array);
PRIVATE(skJsonBool) Serializer_serialize_object(Serializer* serializer, skVec* table);

/* Json Serializer, holds buffer to the serialized data, its length,
 * current offset in the buffer, depth for pretty printing (which is not
 * yet supported), expand flag to indicate that during serialization buffer
 * can realloc/expand and user_provided flag that serves as a guard to not
 * free the buffer passed in by user. */
struct _Serializer {
    unsigned char* buffer;
    size_t         length;
    size_t         offset;
    size_t         depth;
    skJsonBool           expand;
    skJsonBool           user_provided;
};

PUBLIC(skJson) skJson_parse(char* buff, size_t bufsize)
{
    skScanner* scanner;
    skJson json;

    json.type = SK_NONE_NODE;

    if(is_null(buff) || bufsize == 0 || is_null(scanner = skScanner_new(buff, bufsize))) {
        return json;
    }

    /* Fetch first token */
    skScanner_next(scanner); 
    /* Construct the parse tree */
    json = skJsonNode_parse(scanner, NULL);
    /* We are done scanning */
    free(scanner);

    return json;
}

PUBLIC(const char*) skJson_error(const skJson* json)
{
    if(valid_with_type(json, SK_ERROR_NODE)) {
        return json->data.j_err;
    } else {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
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
    skJson* old_node;
    size_t  idx;

    if(is_null(json) || json->type == SK_NONE_NODE) {
        return;
    }

    if(is_some((arena = json->parent_arena.ptr))) {
        switch(json->parent_arena.type) {
            /* In case we are directly dropping value of the array_node, replace
             * it with null node instead. Actually removing the entry from array node
             * is done using array node functions defined on it. */
            case SK_ARRAY_NODE:
                idx = ((char*) json - (char*) skVec_inner_unsafe((skVec*)arena)) 
                    / sizeof(skJson);

                null_node = RawNode_new(SK_NULL_NODE, NULL);
                copylink(&null_node, json);

                old_node = skVec_index((skVec*) arena, idx);
                skJsonNode_drop(old_node);
                memcpy(old_node, &null_node, sizeof(skJson));

                break;
            case SK_OBJECT_NODE:
                /* If this 'json' node is a child of object node then replace it
                 * with null node instead, in order to remove an entry from object node
                 * you must use the functions defined on the object node. That is the
                 * only way to soundly drop the key/value pair and preserve json validity. */
                idx = ((char*) json - (char*) skVec_inner_unsafe((skVec*)arena)) 
                    / sizeof(skObjTuple);

                null_node = RawNode_new(SK_NULL_NODE, NULL);
                copylink(&null_node, json);

                /* Returning skObjTuple from the vec but we need only node so cast it 
                 * into skJson node and drop it. */
                old_node = skVec_index((skVec*) arena, idx);
                skJsonNode_drop(old_node);
                memcpy(old_node, &null_node, sizeof(skJson));

                break;
            default:
#ifdef SK_ERRMSG
                THROW_ERR(UnreachableCode);
#endif
#ifdef SK_DBUG
                assert(false);
#endif
                break;
        }
    } else {
        skJsonNode_drop(json);
        unlink_parent(json);
        json->type = SK_NONE_NODE;
    }

}

PUBLIC(int) skJson_type(const skJson* json)
{
    return (is_some(json)) ? (int) json->type : -1;
}

PUBLIC(skJsonBool) skJson_parent(const skJson* json)
{
    return is_some(json) && is_some(json->parent_arena.ptr);
}

PUBLIC(int) skJson_parent_type(const skJson* json)
{
    if(is_null(json)) {
        return -1;
    }

    return json->parent_arena.type;
}

PUBLIC(long int) skJson_integer_value(const skJson* json, int* cntrl)
{
    if(!valid_with_type(json, SK_INT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        *cntrl = -1;
        return 0;
    }

    *cntrl = 0;
    return json->data.j_int;
}

PUBLIC(double) skJson_double_value(const skJson* json, int* cntrl)
{
    if(!valid_with_type(json, SK_DOUBLE_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        *cntrl = -1;
        return 0;
    }

    *cntrl = 0;
    return json->data.j_double;
}

PUBLIC(skJsonBool) skJson_bool_value(const skJson* json)
{
    if(!valid_with_type(json, SK_BOOL_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return -1;
    }

    return json->data.j_boolean;
}

PUBLIC(char*) skJson_string_value(const skJson* json)
{
    if(!valid_with_type(json, SK_STRING_NODE) && json->type != SK_REFERENCE_NODE) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    }

    return strdup_ansi(json->data.j_string);
}

PUBLIC(char*) skJson_string_ref_unsafe(const skJson* json)
{
    if(!valid_with_type(json, SK_STRING_NODE) && json->type != SK_REFERENCE_NODE) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    }

    return json->data.j_string;
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
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
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
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    }

    drop_nonprim_elements(json);

    json->data.j_double = n;
    json->type          = SK_DOUBLE_NODE;

    return json;
}

PUBLIC(skJson*) skJson_transform_into_bool(skJson* json, skJsonBool boolean)
{
    if(is_null(json)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    }

    drop_nonprim_elements(json);

    json->data.j_boolean = boolean;
    json->type           = SK_BOOL_NODE;

    return json;
}

PUBLIC(skJson*) skJson_transform_into_ref(skJson* json, const char* string_ref)
{
    skStrSlice slice;

    if(is_null(json)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    }

    slice = skSlice_new(string_ref, strlen(string_ref) - 1);

    if(!skJsonString_isvalid(&slice)) {
#ifdef SK_ERRMSG
        THROW_ERR(InvalidString);
#endif
        return NULL;
    }

    drop_nonprim_elements(json);

    json->data.j_string = discard_const(string_ref);
    json->type          = SK_REFERENCE_NODE;

    return json;
}

PUBLIC(skJson*) skJson_transform_into_string(skJson* json, const char* string)
{
    skStrSlice slice;
    char*      new_str;

    if(is_null(json)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    }

    slice = skSlice_new(string, strlen(string) - 1);

    if(!skJsonString_isvalid(&slice)) {
#ifdef SK_ERRMSG
        THROW_ERR(InvalidString);
#endif
        return NULL;
    }

    new_str = strdup_ansi(string);

    if(is_null(new_str)) {
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
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
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
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    }

    if(is_null(table = skVec_new(sizeof(skObjTuple)))) {
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

PUBLIC(skJsonBool) skJson_integer_set(skJson* json, long int n)
{
    if(!valid_with_type(json, SK_INT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    json->data.j_int = n;
    return true;
}

PUBLIC(skJson) skJson_double_new(double n)
{
    return DoubleNode_new(n, NULL);
}

PUBLIC(skJsonBool) skJson_double_set(skJson* json, double n)
{
    if(!valid_with_type(json, SK_DOUBLE_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    json->data.j_double = n;
    return true;
}

PUBLIC(skJson) skJson_bool_new(skJsonBool boolean)
{
    return BoolNode_new(boolean, NULL);
}

PUBLIC(skJsonBool) skJson_bool_set(skJson* json, skJsonBool boolean)
{
    if(!valid_with_type(json, SK_BOOL_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
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
#ifdef SK_ERRMSG
        THROW_ERR(InvalidString);
#endif
        return string_node;
    }

    return StringNode_new(discard_const(string), type, parent);
}

PUBLIC(skJson) skJson_string_new(const char* string)
{
    return skJson_string_new_internal(string, SK_STRING_NODE, NULL);
}

PUBLIC(skJsonBool) skJson_string_set(skJson* json, const char* string)
{
    char*      new_str;
    skStrSlice slice;

    if(!valid_with_type(json, SK_STRING_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    slice = skSlice_new(string, strlen(string) - 1);

    if(!skJsonString_isvalid(&slice)) {
#ifdef SK_ERRMSG
        THROW_ERR(InvalidString);
#endif
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

PUBLIC(skJson) skJson_ref_new(const char* string)
{
    return skJson_string_new_internal(string, SK_REFERENCE_NODE, NULL);
}

PUBLIC(skJsonBool) skJson_ref_set(skJson* json, const char* string)
{
    skStrSlice slice;

    if(!valid_with_type(json, SK_REFERENCE_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    slice = skSlice_new(string, strlen(string) - 1);

    if(!skJsonString_isvalid(&slice)) {
#ifdef SK_ERRMSG
        THROW_ERR(InvalidString);
#endif
        return false;
    }

    json->data.j_string = discard_const(string);
    return true;
}

PUBLIC(skJson) skJson_array_new(void)
{
    return ArrayNode_new(NULL);
}

PRIVATE(skJson) skJson_constructor_internal(
        void* val,
        skNodeType type,
        skJson* parent)
{
    skJson node;

    switch(type) {
        case SK_INT_NODE:
            node = IntNode_new(*(long int*) val, parent);
            break;
        case SK_DOUBLE_NODE:
            node = DoubleNode_new(*(double*) val, parent);
            break;
        case SK_BOOL_NODE:
            node = BoolNode_new(*(skJsonBool*) val, parent);
            break;
        case SK_NULL_NODE:
            node = RawNode_new(SK_NULL_NODE, parent);
            break;
        case SK_STRING_NODE:
        case SK_REFERENCE_NODE:
            node = skJson_string_new_internal((const char*) val, type, parent);
            break;
        case SK_OBJECT_NODE:
            node = ObjectNode_new(parent);
            break;
        case SK_ARRAY_NODE:
            node = ArrayNode_new(parent);
            break;
        default:
#ifdef SK_ERRMSG
            THROW_ERR(UnreachableCode);
            assert(false);
#endif
            break;
    }

    return node;
}

PRIVATE(skJsonBool) skJson_array_insert_internal(
        skJson* parent,
        const void* val,
        skNodeType type,
        size_t index,
        skJsonBool push,
        skJsonBool element)
{
    skJson node;
    skJsonBool fail;
    
    fail = false;

    if(!element) {
        node = skJson_constructor_internal(discard_const(val), type, parent);
        if(node.type == SK_ERROR_NODE) {
            return false;
        }
    } else {
        node = *(skJson*) val;
        link_parent(&node, parent);
    }

    if(push) {
        fail = !skVec_push(parent->data.j_array, &node);
    } else {
        fail = !skVec_insert(parent->data.j_array, &node, index);
    }

    if(fail) {
        if(element) {
            unlink_parent(&node);
        } else {
            skJsonNode_drop(&node);
        }
        return false;
    }

    return true;
}

PUBLIC(skJsonBool) skJson_array_push_str(skJson* json, const char* string)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, string, SK_STRING_NODE, 0, true, false);
}

PUBLIC(skJsonBool) skJson_array_insert_str(skJson* json, const char* string, size_t index)
{
    if(!valid_with_type(json, SK_ARRAY_NODE) || is_null(string)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, string, SK_STRING_NODE, index, false, false);
}


PUBLIC(skJsonBool) skJson_array_push_ref(skJson* json, const char* string)
{
    if(!valid_with_type(json, SK_ARRAY_NODE) || is_null(string)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, string, SK_REFERENCE_NODE, 0, true, false);
}

PUBLIC(skJsonBool) skJson_array_insert_ref(skJson* json, const char* string, size_t index)
{
    if(!valid_with_type(json, SK_ARRAY_NODE) || is_null(string)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, string, SK_REFERENCE_NODE, index, false, false);
}

PUBLIC(skJsonBool) skJson_array_push_int(skJson* json, long int n)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, &n, SK_INT_NODE, 0, true, false);
}

PUBLIC(skJsonBool) skJson_array_insert_int(skJson* json, long int n, size_t index)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, &n, SK_INT_NODE, index, false, false);
}

PUBLIC(skJsonBool) skJson_array_push_double(skJson* json, double n)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, &n, SK_DOUBLE_NODE, 0, true, false);
}

PUBLIC(skJsonBool) skJson_array_insert_double(skJson* json, double n, size_t index)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, &n, SK_DOUBLE_NODE, index, false, false);
}

PUBLIC(skJsonBool) skJson_array_push_bool(skJson* json, skJsonBool boolean)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, &boolean, SK_BOOL_NODE, 0, true, false);
}

PUBLIC(skJsonBool) skJson_array_insert_bool(skJson* json, skJsonBool boolean, size_t index)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, &boolean, SK_BOOL_NODE, index, false, false);
}

PUBLIC(skJsonBool) skJson_array_push_null(skJson* json)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, NULL, SK_NULL_NODE, 0, true, false);
}

PUBLIC(skJsonBool) skJson_array_insert_null(skJson* json, size_t index)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, NULL, SK_NULL_NODE, index, false, false);
}

PUBLIC(skJsonBool) skJson_array_push_element(skJson* json, skJson* element)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, element, SK_NONE_NODE, 0, true, true);
}

PUBLIC(skJsonBool) skJson_array_insert_element(skJson* json, skJson* element, size_t index)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_array_insert_internal(json, element, SK_NONE_NODE, index, false, true);
}


PRIVATE(skJson) skJson_array_from_cstrs(
        const char* const* strings,
        size_t count,
        CStrPush add_fn)
{
    skJson arr_node;

    if(is_null(strings)) {
        arr_node.type = SK_NONE_NODE;
        return arr_node;
    }

    if((arr_node = ArrayNode_new(NULL)).type == SK_ERROR_NODE) {
        return arr_node;
    }

    while(count--) {
        if(is_null(*strings) || !add_fn(&arr_node, *strings++)) {
            skJsonNode_drop(&arr_node);
            arr_node.data.j_array = NULL;
            arr_node.type = SK_ERROR_NODE;
            return arr_node;
        }
    }

    return arr_node;
}

PUBLIC(skJson) skJson_array_from_strings(const char* const* strings, size_t count)
{
    return skJson_array_from_cstrs(
            strings,
            count,
            (CStrPush) skJson_array_push_str);
}

PUBLIC(skJson) skJson_array_from_refs(const char* const* strings, size_t count)
{
    return skJson_array_from_cstrs(
            strings,
            count,
            (CStrPush) skJson_array_push_ref);
}

PUBLIC(skJson) skJson_array_from_integers(const int* integers, size_t count)
{
    skJson arr_node;

    if(is_null(integers)) {
        arr_node.type = SK_NONE_NODE;
        return arr_node;
    }

    if((arr_node = ArrayNode_new(NULL)).type == SK_ERROR_NODE) {
        return arr_node;
    }

    while(count--) {
        if(!skJson_array_push_int(&arr_node, *integers++)) {
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

    if(is_null(doubles)) {
        arr_node.type = SK_NONE_NODE;
        return arr_node;
    }

    if((arr_node = ArrayNode_new(NULL)).type == SK_ERROR_NODE) {
        return arr_node;
    }

    while(count--) {
        if(!skJson_array_push_double(&arr_node, *doubles++)) {
            skJsonNode_drop(&arr_node);
            arr_node.data.j_array = NULL;
            arr_node.type = SK_ERROR_NODE;
            return arr_node;
        }
    }

    return arr_node;
}

PUBLIC(skJson) skJson_array_from_booleans(const skJsonBool* booleans, size_t count)
{
    skJson arr_node;

    if(is_null(booleans)) {
        arr_node.type = SK_BOOL_NODE;
        return arr_node;
    }

    if((arr_node = ArrayNode_new(NULL)).type == SK_ERROR_NODE) {
        return arr_node;
    }

    while(count--) {
        if(!skJson_array_push_bool(&arr_node, *booleans++)) {
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

/* Helper function for 'skJson_array_from_elements'. */
PRIVATE(skJsonBool) array_push_node_checked(skJson* json, skJson* node)
{
    link_parent(node, json);
    if(!skVec_push(json->data.j_array, node)) {
        unlink_parent(node);
        return false;
    }

    return true;
}

PUBLIC(skJson) skJson_array_from_elements(const skJson* const* elements, size_t count)
{
    skJson arr_node;

    if(is_null(elements)) {
        arr_node.type = SK_NONE_NODE;
        return arr_node;
    }

    if((arr_node = ArrayNode_new(NULL)).type == SK_ERROR_NODE) {
        return arr_node;
    }

    while(count--) {
        if(is_null(*elements)
           || !array_push_node_checked(&arr_node, discard_const(*elements++)))
        {
            skJsonNode_drop(&arr_node);
            arr_node.data.j_array = NULL;
            arr_node.type = SK_NONE_NODE;
            return arr_node;
        }
    }

    return arr_node;
}

PUBLIC(skJsonBool) skJson_array_pop(skJson* json, skJson* element)
{
    skJson popped;

    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    if(!skVec_pop(json->data.j_array, &popped)) {
        return false;
    }

    unlink_parent(&popped);
    *element = popped;
    return true;
}

PUBLIC(skJsonBool) skJson_array_remove(skJson* json, size_t index)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skVec_remove(json->data.j_array, index, (FreeFn) skJsonNode_drop);
}

PUBLIC(size_t) skJson_array_len(skJson* json)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return 0;
    }

    return skVec_len(json->data.j_array);
}

PUBLIC(skJson*) skJson_array_front(skJson* json)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    }

    return skVec_front(json->data.j_array);
}

PUBLIC(skJson*) skJson_array_back(skJson* json)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    }

    return skVec_back(json->data.j_array);
}

PUBLIC(skJson*) skJson_array_index(skJson* json, size_t index)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    }

    return skVec_index(json->data.j_array, index);
}

PUBLIC(void) skJson_array_clear(skJson* json)
{
    if(!valid_with_type(json, SK_ARRAY_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return;
    }

    skVec_clear(json->data.j_array, (FreeFn) skJsonNode_drop);
}

PUBLIC(skJson) skJson_object_new(void)
{
    return ObjectNode_new(NULL);
}

PUBLIC(skJsonBool) skJson_object_sort(skJson* json)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skVec_sort(json->data.j_object, (CmpFn) compare_tuples);
}

PUBLIC(skJsonBool) skJson_object_sort_by(skJson* json, CmpFn cmp)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skVec_sort(json->data.j_object, cmp);
}

PUBLIC(skJsonBool) skJson_object_is_sorted(skJson* json)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skVec_is_sorted(json->data.j_object, (CmpFn) compare_tuples);
}

PUBLIC(skJsonBool) skJson_object_is_sorted_by(skJson* json, CmpFn cmp)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skVec_is_sorted(json->data.j_object, cmp);
}

PRIVATE(int) compare_tuples(const skObjTuple* a, const skObjTuple* b)
{
    return strcmp(a->key, b->key);
}

PRIVATE(skJsonBool) skJson_object_insert_internal(
        skJson* parent,
        const char* key,
        const void* val,
        skNodeType type,
        size_t index,
        skJsonBool push,
        skJsonBool element)
{
    skObjTuple tuple;
    skJsonBool fail;
    
    fail = false;

    if(!element) {
        tuple.value = skJson_constructor_internal(discard_const(val), type, parent);
        if(tuple.value.type == SK_NONE_NODE) {
            return false;
        }
    } else {
        tuple.value = * (skJson*) val;
        link_parent(&tuple.value, parent);
    }

    if(is_null(tuple.key = strdup_ansi(key))) {
        if(element) {
            unlink_parent(&tuple.value);
        } else {
            skJsonNode_drop(&tuple.value);
        }
        return false;
    }

    if(push) {
        fail = !skVec_push(parent->data.j_object, &tuple);
    } else {
        fail = !skVec_insert(parent->data.j_object, &tuple, index);
    }

    if(fail) {
        if(element) {
            unlink_parent(&tuple.value);
        } else {
            skJsonNode_drop(&tuple.value);
        }
        free(tuple.key);
        return false;
    }

    return true;
}

PUBLIC(skJsonBool)
skJson_object_insert_element(
    skJson*         json,
    const char*     key,
    skJson*         element,
    size_t          index)
{
    if(!valid_with_type(json, SK_OBJECT_NODE) || has_parent(element)) 
    {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, element, SK_NONE_NODE, index, false, true); 
}

PUBLIC(skJsonBool)
skJson_object_push_element(skJson* json, const char* key, skJson* element)
{
    if(!valid_with_type(json, SK_OBJECT_NODE) || has_parent(element)) 
    {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, element, SK_NONE_NODE, 0, true, true); 
}

PUBLIC(skJsonBool) skJson_object_insert_int(skJson* json, const char *key, long int n, size_t index)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, &n, SK_INT_NODE, index, false, false);
}

PUBLIC(skJsonBool) skJson_object_push_int(skJson* json, const char *key, long int n)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, &n, SK_INT_NODE, 0, true, false);
}

PUBLIC(skJsonBool) skJson_object_insert_double(skJson* json, const char *key, double n, size_t index)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, &n, SK_DOUBLE_NODE, index, false, false);
}

PUBLIC(skJsonBool) skJson_object_push_double(skJson* json, const char *key, double n)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, &n, SK_DOUBLE_NODE, 0, true, false);
}

PUBLIC(skJsonBool) skJson_object_insert_bool(skJson* json, const char *key, skJsonBool boolean, size_t index)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, &boolean, SK_BOOL_NODE, index, false, false);
}

PUBLIC(skJsonBool) skJson_object_push_bool(skJson* json, const char *key, skJsonBool boolean)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, &boolean, SK_BOOL_NODE, 0, true, false);
}

PUBLIC(skJsonBool) skJson_object_push_null(skJson* json, const char *key)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, NULL, SK_NULL_NODE, 0, true, false);
}

PUBLIC(skJsonBool) skJson_object_insert_null(skJson* json, const char *key, size_t index)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, NULL, SK_NULL_NODE, index, false, false);
}

PUBLIC(skJsonBool) skJson_object_insert_ref(
        skJson* json,
        const char *key,
        const char* ref,
        size_t index)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, ref, SK_REFERENCE_NODE, index, false, false);
}

PUBLIC(skJsonBool) skJson_object_push_ref(skJson* json, const char *key, const char* ref)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, ref, SK_REFERENCE_NODE, 0, true, false);
}

PUBLIC(skJsonBool) skJson_object_insert_string(
        skJson* json,
        const char *key,
        const char* ref,
        size_t index)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, ref, SK_STRING_NODE, index, false, false);
}

PUBLIC(skJsonBool) skJson_object_push_string(skJson* json, const char *key, const char* ref)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skJson_object_insert_internal(json, key, ref, SK_STRING_NODE, 0, true, false);
}

PUBLIC(skJsonBool) skJson_object_remove(skJson* json, size_t index)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    return skVec_remove(json->data.j_object, index, (FreeFn) skObjTuple_drop);
}

PUBLIC(skJsonBool) skJson_object_pop(skJson* json, skObjTuple* tuple)
{
    skObjTuple popped;

    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    if(!skVec_pop(json->data.j_object, &popped)) {
        return false;
    }

    unlink_parent(&popped.value);
    *tuple = popped;
    return true;
}

PUBLIC(skJsonBool)
skJson_object_remove_by_key(
    skJson*     json,
    const char* key,
    skJsonBool        sorted)
{
    skObjTuple dummy_tuple;

    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return false;
    }

    dummy_tuple.key = discard_const(key);

    return skVec_remove_by_key(
        json->data.j_object,
        &dummy_tuple,
        (CmpFn) compare_tuples,
        (FreeFn) skObjTuple_drop,
        sorted);
}

PUBLIC(skObjTuple*) skJson_object_index(const skJson* json, size_t index)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    }

    return skVec_index(json->data.j_object, index);
}

PUBLIC(skObjTuple*)
skJson_object_index_by_key(
    const skJson* json,
    const char*       key,
    skJsonBool              sorted)
{
    skObjTuple dummy_tuple;

    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    }

    dummy_tuple.key = discard_const(key);

    return skVec_get_by_key(
        json->data.j_object,
        &dummy_tuple,
        (CmpFn) compare_tuples,
        sorted);
}

PUBLIC(skObjTuple*)
skJson_object_index_by_cmp(
    const skJson* json,
    const char*       key,
    CmpFn cmp)
{
    skObjTuple dummy_tuple;

    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    } else if(is_null(cmp)) {
#ifdef SK_ERRMSG
        THROW_ERR(MissingComparisonFn);
#endif
        return NULL;
    }

    dummy_tuple.key = discard_const(key);

    return skVec_get_by_key(
        json->data.j_object,
        &dummy_tuple,
        cmp,
        true);
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

PUBLIC(char*) skJson_objtuple_key_ref_unsafe(const skObjTuple* tuple)
{
    if(is_null(tuple)) {
        return NULL;
    }

    return tuple->key;
}

PUBLIC(size_t) skJson_object_len(const skJson* json)
{
    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return 0;
    }

    return skVec_len(json->data.j_object);
}

PUBLIC(skJsonBool)
skJson_object_contains(const skJson* json, const char* key, skJsonBool sorted)
{
    skObjTuple dummy_tuple;

    if(!valid_with_type(json, SK_OBJECT_NODE)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
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
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return;
    }

    skVec_clear(json->data.j_object, (FreeFn) skJsonNode_drop);
}

PRIVATE(Serializer) Serializer_new(size_t bufsize, skJsonBool expand)
{
    Serializer     pbuf;
    unsigned char* buffer;

    memset(&pbuf, 0, sizeof(Serializer));

    if(bufsize == 0) {
        bufsize = BUFSIZ;
    }

    if(is_null(buffer = malloc(bufsize))) {
#ifdef SK_ERRMSG
        THROW_ERR(OutOfMemory);
#endif
        return pbuf;
    }

    pbuf.length = bufsize;
    pbuf.buffer = buffer;
    pbuf.expand = expand;

    return pbuf;
}

PRIVATE(Serializer) Serializer_from(unsigned char* buffer, size_t bufsize, skJsonBool expand)
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
#ifdef SK_ERRMSG
        THROW_ERR(AllocationTooLarge);
#endif
        if(!serializer->user_provided) {
            Serializer_drop(serializer);
        }
        return NULL;
    }

    needed += serializer->offset + 1;

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
#ifdef SK_ERRMSG
            THROW_ERR(AllocationTooLarge);
#endif
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
    serializer->offset += strlen((char*) serializer->buffer + serializer->offset);
}

PUBLIC(unsigned char*)
skJson_serialize_with_buffer(skJson* json, unsigned char* buffer, size_t size, skJsonBool expand)
{
    Serializer serializer;

    if(is_null(json) || is_null(buffer) || size == 0) {
        return NULL;
    }

    if(err_or_none(json)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
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
skJson_serialize_with_bufsize(skJson* json, size_t size, skJsonBool expand)
{
    Serializer serializer;

    if(is_null(json) || size == 0) {
        return NULL;
    }

    if(err_or_none(json)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
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

    if(err_or_none(json)) {
#ifdef SK_ERRMSG
        THROW_ERR(WrongNodeType);
#endif
        return NULL;
    }

    if(is_null((serializer = Serializer_new(0, true)).buffer)) {
        return NULL;
    }

    if(!Serializer_serialize(&serializer, json)) {
        return NULL;
    }

    return serializer.buffer;
}

PRIVATE(skJsonBool) Serializer_serialize(Serializer* serializer, skJson* json)
{
#ifdef SK_DBUG
    assert(is_some(json));
    assert(is_some(serializer));
    assert(is_some(serializer->buffer));
#endif
    switch(json->type) {
        case SK_STRING_NODE:
        case SK_REFERENCE_NODE:
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
            return Serializer_serialize_object(serializer, json->data.j_object);
        case SK_ERROR_NODE:
        default:
#ifdef SK_ERRMSG
            THROW_ERR(SerializerInvalidJson);
#endif
            return false;
    }
}

PRIVATE(skJsonBool) Serializer_serialize_number(Serializer* serializer, skJson* json)
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
#ifdef SK_ERRMSG
        THROW_ERR(SerializerNumberError);
#endif
        return false;
    }

    out = Serializer_buffer_ensure(serializer, len + sizeof(""));

    if(is_null(out)) {
        return false;
    }

    memcpy((char*) out, buff, len);
    out[len + 1] = '\0';
    serializer->offset += len;

    return true;
}

PRIVATE(skJsonBool) Serializer_serialize_string(Serializer* serializer, const char* str)
{
    unsigned char* out;
    size_t         len;
#ifdef SK_DBUG
    assert(is_some(serializer));
    assert(is_some(serializer->buffer));
#endif
    len = strlen(str);
    out = Serializer_buffer_ensure(serializer, len + sizeof("\"\""));

    if(is_null(out)) {
        return false;
    }

    out[0] = '\"';
    memcpy(out + 1, str, len);
    out[len + 1] = '\"';
    out[len + 2] = '\0';

    Serializer_offset_update(serializer);

    return true;
}

PRIVATE(skJsonBool) Serializer_serialize_bool(Serializer* serializer, skJsonBool boolean)
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
    return true;
}

PRIVATE(skJsonBool) Serializer_serialize_null(Serializer* serializer)
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

PRIVATE(skJsonBool) Serializer_serialize_array(Serializer* serializer, skVec* vec)
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
            if(is_null(out = Serializer_buffer_ensure(serializer, 1))) {
                return false;
            }
            *out = ',';
            serializer->offset++;
        }
    }

    if(is_null(out = Serializer_buffer_ensure(serializer, 2))) {
        return false;
    }

    *out++ = ']';
    *out   = '\0';
    serializer->depth--;
    serializer->offset++;

    return true;
}

PRIVATE(skJsonBool) Serializer_serialize_object(Serializer* serializer, skVec* table)
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

        *out  = ':';
        serializer->offset++;

        if(!Serializer_serialize(serializer, &tuple->value)) {
            return false;
        }

        if(is_null(out = Serializer_buffer_ensure(serializer, 2))) {
            return false;
        }

        if(i + 1 != len) {
            *out = ',';
            serializer->offset++;
        }
    }

    *out++             = '}';
    *out               = '\0';
    serializer->offset++;
    serializer->depth--;

    return true;
}
