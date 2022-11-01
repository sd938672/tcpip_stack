#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include "../../graph.h"
#include "object_group.h"

#define HASH_PRIME_CONST    5381

static unsigned int
hashfromkey(void *key) {

    unsigned char *str = (unsigned char *)key;
    unsigned int hash = HASH_PRIME_CONST;
    int c;

    while (c = *str++)
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash;
}

static int
equalkeys(void *k1, void *k2)
{
    char *ky1 = (char *)k1;
    char *ky2 = (char *)k2;
    int len1 = strlen(ky1);
    int len2 = strlen(ky2);
    if (len1 != len2) return len1 - len2;
    return (0 == memcmp(k1,k2, len1));
}

hashtable_t *object_group_create_new_ht() {

    hashtable_t *h = create_hashtable(128, hashfromkey, equalkeys);
    assert(h);
    return h;
}

object_group_t *
object_group_lookup_ht_by_name (node_t *node, 
                                                          hashtable_t *ht,
                                                          c_string og_name) { 

    return (object_group_t *)hashtable_search(ht, (void *)og_name);
}

object_group_t *
object_group_remove_from_ht_by_name (node_t *node, hashtable_t *ht, c_string og_name) {

    return (object_group_t *)hashtable_remove(ht, (void *)og_name);
}

bool
object_group_remove_from_ht (node_t *node, hashtable_t *ht, object_group_t *og) {

    object_group_t *og1 = (object_group_t *)hashtable_remove(ht, (void *)og->og_name);
    if (!og1) return false;
    assert(og == og1);
    return true;
}

bool
object_group_insert_into_ht (node_t *node, hashtable_t *ht,  object_group_t *og) {

    c_string key = (c_string) calloc (OBJ_GRP_NAME_LEN, sizeof(byte));
    string_copy (key, og->og_name, OBJ_GRP_NAME_LEN);
    key[OBJ_GRP_NAME_LEN] = '\0';

    if (!hashtable_insert (ht, (void *)key, (void *)og)) {
        return false;
    }
    return true;
}

object_group_t *
object_group_malloc (const c_string name, og_type_t og_type) {

    object_group_t *og;
    og = (object_group_t *)XCALLOC(0, 1, object_group_t);
    og->cycle_det_id = 0;
    string_copy(og->og_name, name, OBJ_GRP_NAME_LEN);
    og->og_ref_count = 0;
    og->og_desc = NULL;
    og->og_type = og_type;
    init_glthread(&og->parent_og_list_head);
    return og;
}

void
object_group_bind (object_group_t *p_og, object_group_t *c_og) {

    assert(p_og->og_type == OBJECT_GRP_NESTED);

    obj_grp_list_node_t *obj_grp_list_node = (obj_grp_list_node_t *)XCALLOC(0, 1, obj_grp_list_node_t);
    obj_grp_list_node->og = c_og;
    glthread_add_last(&p_og->u.nested_og_list_head, &obj_grp_list_node->glue);

    obj_grp_list_node = (obj_grp_list_node_t *)XCALLOC(0, 1, obj_grp_list_node_t);
    obj_grp_list_node->og = p_og;
    glthread_add_last(&c_og->parent_og_list_head, &obj_grp_list_node->glue);
}

c_string
object_group_network_construct_name (
                                        og_type_t og_type, 
                                        uint32_t ip_addr1,
                                        uint32_t ip_addr2,
                                        c_string output) {

    byte ip_addr[16];
    byte ip_addr3[16];

    memset(output, 0, OBJ_GRP_NAME_LEN);

    switch(og_type) {
        case OBJECT_GRP_NET_ADDR:
        case OBJECT_GRP_NET_RANGE:
            snprintf(output, OBJ_GRP_NAME_LEN, "%s-%s-%s",
                 object_group_type_str(og_type),
                 tcp_ip_covert_ip_n_to_p(ip_addr1, ip_addr),
                 tcp_ip_covert_ip_n_to_p(ip_addr2, ip_addr3));
            break;
        case OBJECT_GRP_NET_HOST:
            snprintf(output, OBJ_GRP_NAME_LEN, "%s-%s", 
                 object_group_type_str(og_type),
                tcp_ip_covert_ip_n_to_p(ip_addr1, ip_addr));
            break;
        case OBJECT_GRP_NESTED:
            assert(0);
            break;
    }
    return output;
}

object_group_t *
object_group_find_child_object_group (object_group_t *og, c_string obj_grp_name) {

    glthread_t *curr;
    obj_grp_list_node_t *obj_grp_list_node;

    if (og->og_type != OBJECT_GRP_NESTED) return NULL;

    ITERATE_GLTHREAD_BEGIN(&og->u.nested_og_list_head, curr) {

        obj_grp_list_node = glue_to_obj_grp_list_node(curr);
        if (string_compare (obj_grp_list_node->og->og_name, 
                                        obj_grp_name,
                                        OBJ_GRP_NAME_LEN) == 0) {
            return obj_grp_list_node->og;
        }
    } ITERATE_GLTHREAD_END(&og->u.nested_og_list_head, curr) 
    return NULL;
}

void
object_group_display (node_t *node, object_group_t *og) {

    glthread_t *curr;
    obj_grp_list_node_t *obj_grp_list_node;

    printf ("OG : %s\n", og->og_name);

    ITERATE_GLTHREAD_BEGIN(&og->u.nested_og_list_head, curr) {

        obj_grp_list_node = glue_to_obj_grp_list_node(curr);
        printf ("  C-OG : %s\n", obj_grp_list_node->og->og_name);

    } ITERATE_GLTHREAD_END(&og->u.nested_og_list_head, curr);

    ITERATE_GLTHREAD_BEGIN(&og->parent_og_list_head, curr) {

        obj_grp_list_node = glue_to_obj_grp_list_node(curr);
        printf ("  P-OG : %s\n", obj_grp_list_node->og->og_name);

    } ITERATE_GLTHREAD_END(&og->parent_og_list_head, curr);

}

void 
object_group_hashtable_print(node_t *node, hashtable_t *ht) {
    
    unsigned int count;
    struct hashtable_itr *itr;

    count = hashtable_count(ht);

    printf("Number of Object Groups : %u\n", count);

    if (!count) return;

    itr = hashtable_iterator(ht);

    do
    {
        char *key = (char *)hashtable_iterator_key(itr);
        object_group_t *og = (object_group_t *)hashtable_iterator_value(itr);
        object_group_display (node, og);
        printf ("\n");
    } while (hashtable_iterator_advance(itr));

    free(itr);
}

/* Check all references of this object group */
bool
object_group_in_use_by_other_og (object_group_t *og) {

    if (og->og_type == OBJECT_GRP_NET_ADDR ||
         og->og_type == OBJECT_GRP_NET_HOST ||
         og->og_type == OBJECT_GRP_NET_RANGE) {

        return false;
    }

    /* Nested og must not have any parent */
    if (!IS_GLTHREAD_LIST_EMPTY(&og->parent_og_list_head)) {
        return true;
    }

    return false;
}

void 
 object_group_free (node_t *node, object_group_t *og) {

    assert(IS_GLTHREAD_LIST_EMPTY(&og->parent_og_list_head));

    if (og->og_type == OBJECT_GRP_NESTED) {
        assert(IS_GLTHREAD_LIST_EMPTY(&og->u.nested_og_list_head));
        assert(!og->og_desc);
    }

    assert(!object_group_remove_from_ht (node, node->object_group_ght, og));

    XFREE(og);
}

void
object_group_delete (node_t *node, object_group_t *og) {

    glthread_t *curr;
    object_group_t *p_og;
    obj_grp_list_node_t *obj_grp_list_node;
    obj_grp_list_node_t *obj_grp_list_node2;

    assert(!object_group_in_use_by_other_og(og));

    if (og->og_type == OBJECT_GRP_NET_ADDR ||
         og->og_type == OBJECT_GRP_NET_HOST ||
         og->og_type == OBJECT_GRP_NET_RANGE) {

        /* Remove Self from its parent Child's list */
        curr = dequeue_glthread_first(&og->parent_og_list_head);
        assert(IS_GLTHREAD_LIST_EMPTY(&og->parent_og_list_head));
        obj_grp_list_node = glue_to_obj_grp_list_node(curr);
        p_og = obj_grp_list_node->og;
        XFREE(obj_grp_list_node);
        obj_grp_list_node = object_group_search_by_ptr (&p_og->u.nested_og_list_head, og);
        assert(obj_grp_list_node);
        remove_glthread(&obj_grp_list_node->glue);
        XFREE(obj_grp_list_node);
        object_group_free (node, og);
        return;
    }

    ITERATE_GLTHREAD_BEGIN(&og->u.nested_og_list_head, curr) {

        obj_grp_list_node = glue_to_obj_grp_list_node(curr);

        if (obj_grp_list_node->og->og_type == OBJECT_GRP_NET_ADDR ||
            obj_grp_list_node->og->og_type == OBJECT_GRP_NET_HOST ||
            obj_grp_list_node->og->og_type == OBJECT_GRP_NET_RANGE) {

            object_group_delete(node, obj_grp_list_node->og);
            continue;
        }

        obj_grp_list_node2 = object_group_search_by_ptr (&obj_grp_list_node->og->parent_og_list_head, og);
        assert(obj_grp_list_node2);
        remove_glthread(&obj_grp_list_node2->glue);
        remove_glthread(&obj_grp_list_node->glue);
        XFREE(obj_grp_list_node);
        XFREE(obj_grp_list_node2);

    } ITERATE_GLTHREAD_END(&og->u.nested_og_list_head, curr);

    if (og->og_desc) {
        XFREE(og->og_desc);
        og->og_desc = NULL;
    }

    assert(object_group_remove_from_ht (node, node->object_group_ght, og));
    object_group_free (node, og);
}

obj_grp_list_node_t *
object_group_search_by_ptr (glthread_t *head, object_group_t *og) {

    glthread_t *curr;
    obj_grp_list_node_t *obj_grp_list_node;

     ITERATE_GLTHREAD_BEGIN(head, curr) {

        obj_grp_list_node = glue_to_obj_grp_list_node(curr);
        if (obj_grp_list_node->og == og) return obj_grp_list_node;

     } ITERATE_GLTHREAD_END(head, curr);
     return NULL;
}

/* Search in c_og's parent list for p_og, and remove reference */
void
object_group_unbind_parent (object_group_t *p_og, object_group_t *c_og) {

    glthread_t *curr;
    obj_grp_list_node_t *obj_grp_list_node;

    assert(p_og->og_type == OBJECT_GRP_NESTED);

    obj_grp_list_node = object_group_search_by_ptr(&c_og->parent_og_list_head, p_og);

    remove_glthread(&obj_grp_list_node->glue);

    XFREE(obj_grp_list_node);
}

/* Search in p_og's child's list for c_og, and remove reference */
void
object_group_unbind_child (object_group_t *p_og, object_group_t *c_og) {

    glthread_t *curr;
    obj_grp_list_node_t *obj_grp_list_node;

    assert(p_og->og_type == OBJECT_GRP_NESTED);

    obj_grp_list_node = object_group_search_by_ptr(&p_og->u.nested_og_list_head, c_og);

    remove_glthread(&obj_grp_list_node->glue);

    XFREE(obj_grp_list_node);
}

void
object_group_mem_init () {

    MM_REG_STRUCT(0, object_group_t);
    MM_REG_STRUCT(0, obj_grp_list_node_t);
}