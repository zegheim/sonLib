#include <stdlib.h>
#include "sonLib.h"
#include "stPhylogeny.h"
// QuickTree includes
#include "cluster.h"
#include "tree.h"
#include "buildtree.h"

// Helper function to add the stPhylogenyInfo that is normally
// generated during neighbor-joining to a tree that has leaf-labels 0,
// 1, 2, etc.
void addStPhylogenyInfoR(stTree *tree)
{
    stPhylogenyInfo *info = st_calloc(1, sizeof(stPhylogenyInfo));
    stTree_setClientData(tree, info);
    if(stTree_getChildNumber(tree) == 0) {
        int ret;
        ret = sscanf(stTree_getLabel(tree), "%" PRIi64, &info->matrixIndex);
        assert(ret == 1);
    } else {
        info->matrixIndex = -1;
        int64_t i;
        for(i = 0; i < stTree_getChildNumber(tree); i++) {
            addStPhylogenyInfoR(stTree_getChild(tree, i));
        }
    }
}

void stPhylogeny_addStPhylogenyInfo(stTree *tree) {
    addStPhylogenyInfoR(tree);
    stPhylogeny_setLeavesBelow(tree, (stTree_getNumNodes(tree)+1)/2);
}


// Set (and allocate) the leavesBelow and totalNumLeaves attribute in
// the phylogenyInfo for the given tree and all subtrees. The
// phylogenyInfo structure (in the clientData field) must already be
// allocated!
void stPhylogeny_setLeavesBelow(stTree *tree, int64_t totalNumLeaves)
{
    int64_t i, j;
    assert(stTree_getClientData(tree) != NULL);
    stPhylogenyInfo *info = stTree_getClientData(tree);
    for (i = 0; i < stTree_getChildNumber(tree); i++) {
        stPhylogeny_setLeavesBelow(stTree_getChild(tree, i), totalNumLeaves);
    }

    info->totalNumLeaves = totalNumLeaves;
    if (info->leavesBelow != NULL) {
        // leavesBelow has already been allocated somewhere else, free it.
        free(info->leavesBelow);
    }
    info->leavesBelow = st_calloc(totalNumLeaves, sizeof(char));
    if (stTree_getChildNumber(tree) == 0) {
        assert(info->matrixIndex < totalNumLeaves);
        assert(info->matrixIndex >= 0);
        info->leavesBelow[info->matrixIndex] = 1;
    } else {
        for (i = 0; i < totalNumLeaves; i++) {
            for (j = 0; j < stTree_getChildNumber(tree); j++) {
                stPhylogenyInfo *childInfo = stTree_getClientData(stTree_getChild(tree, j));
                info->leavesBelow[i] |= childInfo->leavesBelow[i];
            }
        }
    }
}

static stTree *quickTreeToStTreeR(struct Tnode *tNode) {
    stTree *ret = stTree_construct();
    bool hasChild = false;
    if (tNode->left != NULL) {
        stTree *left = quickTreeToStTreeR(tNode->left);
        stTree_setParent(left, ret);
        hasChild = true;
    }
    if (tNode->right != NULL) {
        stTree *right = quickTreeToStTreeR(tNode->right);
        stTree_setParent(right, ret);
        hasChild = true;
    }
    if (stTree_getClientData(ret) == NULL) {
        // Allocate the phylogenyInfo for this node.
        stPhylogenyInfo *info = st_calloc(1, sizeof(stPhylogenyInfo));
        if (!hasChild) {
            info->matrixIndex = tNode->nodenumber;
        } else {
            info->matrixIndex = -1;
        }
        stTree_setClientData(ret, info);
    }
    stTree_setBranchLength(ret, tNode->distance);

    // Can remove if needed, probably not useful except for testing.
    stTree_setLabel(ret, stString_print("%u", tNode->nodenumber));
    return ret;
}

// Helper function for converting an unrooted QuickTree Tree into an
// stTree, rooted halfway along the longest branch.
static stTree *quickTreeToStTree(struct Tree *tree) {
    struct Tree *rootedTree = get_root_Tnode(tree);
    stTree *ret = quickTreeToStTreeR(rootedTree->child[0]);
    stPhylogeny_setLeavesBelow(ret, (stTree_getNumNodes(ret) + 1) / 2);
    return ret;
}

// Free a stPhylogenyInfo struct
void stPhylogenyInfo_destruct(stPhylogenyInfo *info) {
    assert(info != NULL);
    free(info->leavesBelow);
    free(info);
}

// Clone a stPhylogenyInfo struct
stPhylogenyInfo *stPhylogenyInfo_clone(stPhylogenyInfo *info) {
    stPhylogenyInfo *ret = malloc(sizeof(stPhylogenyInfo));
    memcpy(ret, info, sizeof(stPhylogenyInfo));
    ret->leavesBelow = malloc(ret->totalNumLeaves * sizeof(char));
    memcpy(ret->leavesBelow, info->leavesBelow, ret->totalNumLeaves * sizeof(char));
    return ret;
}

// Free the stPhylogenyInfo struct for this node and all nodes below it.
void stPhylogenyInfo_destructOnTree(stTree *tree) {
    int64_t i;
    stPhylogenyInfo_destruct(stTree_getClientData(tree));
    stTree_setClientData(tree, NULL);
    for (i = 0; i < stTree_getChildNumber(tree); i++) {
        stPhylogenyInfo_destructOnTree(stTree_getChild(tree, i));
    }
}

// Compare a single partition to a single bootstrap partition and
// update its support if they are identical.
static void updatePartitionSupportFromPartition(stTree *partition, stTree *bootstrap) {
    int64_t i, j;
    stPhylogenyInfo *partitionInfo, *bootstrapInfo;
    if (stTree_getChildNumber(partition) != stTree_getChildNumber(bootstrap)) {
        // Can't compare different numbers of partitions
        return;
    }

    partitionInfo = stTree_getClientData(partition);
    bootstrapInfo = stTree_getClientData(bootstrap);
    assert(partitionInfo != NULL);
    assert(bootstrapInfo != NULL);
    assert(partitionInfo->totalNumLeaves == bootstrapInfo->totalNumLeaves);
    // Check if the set of leaves is equal in both partitions. If not,
    // the partitions can't be equal.
    if (memcmp(partitionInfo->leavesBelow, bootstrapInfo->leavesBelow,
            partitionInfo->totalNumLeaves * sizeof(char))) {
        return;
    }

    // The sets of leaves under the nodes are equal; now we need to
    // check that the sets they are partitioned into are equal.
    for (i = 0; i < stTree_getChildNumber(partition); i++) {
        stPhylogenyInfo *childInfo = stTree_getClientData(stTree_getChild(partition, i));
        bool foundPartition = false;
        for (j = 0; j < stTree_getChildNumber(bootstrap); j++) {
            stPhylogenyInfo *bootstrapChildInfo = stTree_getClientData(stTree_getChild(bootstrap, j));
            if (memcmp(childInfo->leavesBelow, bootstrapChildInfo->leavesBelow,
                    partitionInfo->totalNumLeaves * sizeof(char)) == 0) {
                foundPartition = true;
                break;
            }
        }
        if (!foundPartition) {
            return;
        }
    }

    // The partitions are equal, increase the support 
    partitionInfo->numBootstraps++;
}

// Increase a partition's bootstrap support if it appears anywhere in
// the given bootstrap tree.
static void updatePartitionSupportFromTree(stTree *partition, stTree *bootstrapTree)
{
    int64_t i, j;
    stPhylogenyInfo *partitionInfo = stTree_getClientData(partition);
    stPhylogenyInfo *bootstrapInfo = stTree_getClientData(bootstrapTree);
    // This partition should be updated against the bootstrap
    // partition only if none of the bootstrap's children have a leaf
    // set that is a superset of the partition's leaf set.
    bool checkThisPartition = TRUE;

    assert(partitionInfo->totalNumLeaves == bootstrapInfo->totalNumLeaves);
    // Check that the leaves under the partition are a subset of the
    // leaves under the current bootstrap node. This should always be
    // true, since that's checked before running this function
    for(i = 0; i < partitionInfo->totalNumLeaves; i++) {
        if(partitionInfo->leavesBelow[i]) {
            assert(bootstrapInfo->leavesBelow[i]);
        }
    }
    
    for(i = 0; i < stTree_getChildNumber(bootstrapTree); i++) {
        stTree *bootstrapChild = stTree_getChild(bootstrapTree, i);
        stPhylogenyInfo *bootstrapChildInfo = stTree_getClientData(bootstrapChild);
        // If any of the bootstrap's children has a leaf set that is a
        // superset of the partition's leaf set, we should update
        // against that child instead.
        bool isSuperset = TRUE;
        for(j = 0; j < partitionInfo->totalNumLeaves; j++) {
            if(partitionInfo->leavesBelow[j]) {
                if(!bootstrapChildInfo->leavesBelow[j]) {
                    isSuperset = FALSE;
                    break;
                }
            }
        }
        if(isSuperset) {
            updatePartitionSupportFromTree(partition, bootstrapChild);
            checkThisPartition = TRUE;
            break;
        }
    }

    if(checkThisPartition) {
        // This bootstrap partition is the closest candidate. Check
        // the partition against this node.
        updatePartitionSupportFromPartition(partition, bootstrapTree);
    }
}

// Return a new tree which has its partitions scored by how often they
// appear in the bootstrap. This fills in the numBootstraps and
// bootstrapSupport fields of each node. All trees must have valid
// stPhylogenyInfo.
stTree *stPhylogeny_scoreFromBootstrap(stTree *tree, stTree *bootstrap)
{
    int64_t i;
    stTree *ret = stTree_cloneNode(tree);
    stPhylogenyInfo *info = stPhylogenyInfo_clone(stTree_getClientData(tree));
    stTree_setClientData(ret, info);
    // Update child partitions (if any)
    for(i = 0; i < stTree_getChildNumber(tree); i++) {
        stTree_setParent(stPhylogeny_scoreFromBootstrap(stTree_getChild(tree, i), bootstrap), ret);
    }
    updatePartitionSupportFromTree(ret, bootstrap);

    info->bootstrapSupport = ((double) info->numBootstraps) / 1.0;

    return ret;
}

// Return a new tree which has its partitions scored by how often they
// appear in the bootstraps. This fills in the numBootstraps and
// bootstrapSupport fields of each node. All trees must have valid
// stPhylogenyInfo.
stTree *stPhylogeny_scoreFromBootstraps(stTree *tree, stList *bootstraps)
{
    int64_t i;
    stTree *ret = stTree_cloneNode(tree);
    stPhylogenyInfo *info = stPhylogenyInfo_clone(stTree_getClientData(tree));
    stTree_setClientData(ret, info);
    // Update child partitions (if any)
    for(i = 0; i < stTree_getChildNumber(tree); i++) {
        stTree_setParent(stPhylogeny_scoreFromBootstraps(stTree_getChild(tree, i), bootstraps), ret);
    }

    // Check the current partition against all bootstraps
    for(i = 0; i < stList_length(bootstraps); i++) {
        updatePartitionSupportFromTree(ret, stList_get(bootstraps, i));
    }

    info->bootstrapSupport = ((double) info->numBootstraps) / stList_length(bootstraps);
    return ret;
}

// Only one half of the distanceMatrix is used, distances[i][j] for which i > j
// Tree returned is labeled by the indices of the distance matrix and is rooted halfway along the longest branch.
stTree *stPhylogeny_neighborJoin(stMatrix *distances) {
    struct DistanceMatrix *distanceMatrix;
    struct Tree *tree;
    int64_t i, j;
    int64_t numSequences = stMatrix_n(distances);
    assert(numSequences > 2);
    assert(distances != NULL);
    assert(stMatrix_n(distances) == stMatrix_m(distances));
    // Set up the basic QuickTree data structures to represent the sequences.
    // The data structures are only filled in as much as absolutely
    // necessary, so they will probably be invalid for anything but
    // running neighbor-joining.
    struct ClusterGroup *clusterGroup = empty_ClusterGroup();
    struct Cluster **clusters = st_malloc(numSequences * sizeof(struct Cluster *));
    for (i = 0; i < numSequences; i++) {
        struct Sequence *seq = empty_Sequence();
        seq->name = stString_print("%" PRIi64, i);
        clusters[i] = single_Sequence_Cluster(seq);
    }
    clusterGroup->clusters = clusters;
    clusterGroup->numclusters = numSequences;
    // Fill in the QuickTree distance matrix
    distanceMatrix = empty_DistanceMatrix(numSequences);
    for (i = 0; i < numSequences; i++) {
        for (j = 0; j < i; j++) {
            distanceMatrix->data[i][j] = *stMatrix_getCell(distances, i, j);
        }
    }
    clusterGroup->matrix = distanceMatrix;
    // Finally, run the neighbor-joining algorithm.
    tree = neighbour_joining_buildtree(clusterGroup, 0);
    free_ClusterGroup(clusterGroup);
    return quickTreeToStTree(tree);
}

// Get the distance to a leaf from an internal node
static double stPhylogeny_distToLeaf(stTree *tree, int64_t leafIndex) {
    int64_t i;
    stPhylogenyInfo *info = stTree_getClientData(tree);
    (void)info;
    assert(info->leavesBelow[leafIndex]);
    if(stTree_getChildNumber(tree) == 0) {
        return 0.0;
    }
    for(i = 0; i < stTree_getChildNumber(tree); i++) {
        stTree *child = stTree_getChild(tree, i);
        stPhylogenyInfo *childInfo = stTree_getClientData(child);
        if(childInfo->leavesBelow[leafIndex]) {
            return stTree_getBranchLength(child) + stPhylogeny_distToLeaf(child, leafIndex);
        }
    }
    // We shouldn't've gotten here--none of the children have the
    // leaf under them, but this node claims to have the leaf under it!
    assert(false);
    return 0.0/0.0;
}

// Get the distance to a node from an internal node above it. Will
// fail if the target node is not below.
static double stPhylogeny_distToChild(stTree *tree, stTree *target) {
    int64_t i, j;
    stPhylogenyInfo *treeInfo, *targetInfo;
    treeInfo = stTree_getClientData(tree);
    targetInfo = stTree_getClientData(target);
    assert(treeInfo->totalNumLeaves == targetInfo->totalNumLeaves);
    if(memcmp(treeInfo->leavesBelow, targetInfo->leavesBelow,
              treeInfo->totalNumLeaves) == 0) {
        // This node is the target node
        return 0.0;
    }
    for(i = 0; i < stTree_getChildNumber(tree); i++) {
        stTree *child = stTree_getChild(tree, i);
        stPhylogenyInfo *childInfo = stTree_getClientData(child);
        bool childIsSuperset = true;
        // Go through all the children and find one which is above (or
        // is) the target node. (Any node above the target will have a
        // leaf set that is a superset of the target's leaf set.)
        assert(childInfo->totalNumLeaves == treeInfo->totalNumLeaves);
        for(j = 0; j < childInfo->totalNumLeaves; j++) {
            if(targetInfo->leavesBelow[j] && !childInfo->leavesBelow[j]) {
                childIsSuperset = false;
                break;
            }
        }
        if(childIsSuperset) {
            return stPhylogeny_distToChild(child, target) + stTree_getBranchLength(child);
            break;
        }
    }
    // We shouldn't've gotten here--none of the children have the
    // target under them, but this node claims to have the target under it!
    assert(false);
    return 0.0/0.0;
}


// Return the MRCA of the given leaves.
stTree *stPhylogeny_getMRCA(stTree *tree, int64_t leaf1, int64_t leaf2) {
    int64_t i;
    for(i = 0; i < stTree_getChildNumber(tree); i++) {
        stTree *child = stTree_getChild(tree, i);
        stPhylogenyInfo *childInfo = stTree_getClientData(child);
        if(childInfo->leavesBelow[leaf1] && childInfo->leavesBelow[leaf2]) {
            return stPhylogeny_getMRCA(child, leaf1, leaf2);
        }
    }

    // If we've gotten to this point, then this is the MRCA of the leaves.
    return tree;
}

// Find the distance between leaves (given by their index in the
// distance matrix.)
double stPhylogeny_distanceBetweenLeaves(stTree *tree, int64_t leaf1,
                                         int64_t leaf2) {
    stTree *mrca = stPhylogeny_getMRCA(tree, leaf1, leaf2);
    return stPhylogeny_distToLeaf(mrca, leaf1) + stPhylogeny_distToLeaf(mrca, leaf2);
}

// Find the distance between two arbitrary nodes (which must be in the
// same tree, with stPhylogenyInfo attached properly).
double stPhylogeny_distanceBetweenNodes(stTree *node1, stTree *node2) {
    int64_t i;
    stPhylogenyInfo *info1, *info2;
    if(node1 == node2) {
        return 0.0;
    }
    info1 = stTree_getClientData(node1);
    info2 = stTree_getClientData(node2);
    assert(info1->totalNumLeaves == info2->totalNumLeaves);
    // Check if node1 is under node2, vice versa, or if they aren't on
    // the same path to the root
    bool oneAboveTwo = false, twoAboveOne = false, differentSubsets = true;
    for(i = 0; i < info1->totalNumLeaves; i++) {
        if(info1->leavesBelow[i] && info2->leavesBelow[i]) {
            differentSubsets = false;
        } else if(info2->leavesBelow[i] && !info1->leavesBelow[i]) {
            twoAboveOne = true;
            // Technically we can break here, but it's cheap to
            // double-check that everything is correct.
            assert(differentSubsets || oneAboveTwo == false);
        } else if(info1->leavesBelow[i] && !info2->leavesBelow[i]) {
            oneAboveTwo = true;
            assert(differentSubsets || twoAboveOne == false);
        }
    }
    // If differentSubsets is true, then the values of oneAboveTwo and
    // twoAboveOne don't matter; if differentSubsets is false, exactly
    // one should be true.
    assert(differentSubsets || (oneAboveTwo ^ twoAboveOne));

    if(differentSubsets) {
        stTree *parent = node1;
        for(;;) {
            parent = stTree_getParent(parent);
            assert(parent != NULL);
            stPhylogenyInfo *parentInfo = stTree_getClientData(parent);
            assert(parentInfo->totalNumLeaves == info1->totalNumLeaves);
            bool isCommonAncestor = true;
            for(i = 0; i < parentInfo->totalNumLeaves; i++) {
                if((info1->leavesBelow[i] || info2->leavesBelow[i]) &&
                   !parentInfo->leavesBelow[i]) {
                    isCommonAncestor = false;
                }
            }
            if(isCommonAncestor) {
                // Found the MRCA of both nodes
                break;
            }
        }
        return stPhylogeny_distToChild(parent, node1) +
            stPhylogeny_distToChild(parent, node2);
    } else if(oneAboveTwo) {
        return stPhylogeny_distToChild(node1, node2);
    } else {
        assert(twoAboveOne);
        return stPhylogeny_distToChild(node2, node1);
    }
}

// Gets the (leaf) node corresponding to an index in the distance matrix.
stTree *stPhylogeny_getLeafByIndex(stTree *tree, int64_t leafIndex) {
    int64_t i;
    stPhylogenyInfo *info = stTree_getClientData(tree);
    assert(leafIndex < info->totalNumLeaves);
    if(info->matrixIndex == leafIndex) {
        return tree;
    }
    for(i = 0; i < stTree_getChildNumber(tree); i++) {
        stTree *child = stTree_getChild(tree, i);
        stPhylogenyInfo *childInfo = stTree_getClientData(child);
        assert(info->totalNumLeaves == childInfo->totalNumLeaves);
        if(childInfo->leavesBelow[leafIndex]) {
            return stPhylogeny_getLeafByIndex(child, leafIndex);
        }
    }

    // Shouldn't get here if the stPhylogenyInfo is set properly
    return NULL;
}