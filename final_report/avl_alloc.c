
////////////////////////////////////////////////////////
///////////// Data Structures Definitions //////////////
////////////////////////////////////////////////////////

struct node {
	struct node* left;
	struct node* right;
	int height;
	struct pageref* pr;
	struct node* next;
};

typedef struct node node;


//================================================================
//							TREE 								  
//================================================================


// Prints the status of a node, used for verbose
void print_node(node* nd){
	
	if (nd == NULL || nd->pr == NULL){
		printf("NULL!\n");
		return;
	}

	printf("Pointer: %p PR: %p, Address:%p, L:%p, R:%p, H:%d\n", nd, nd->pr, PR_PAGEADDR(nd->pr), nd->left, nd->right, nd->height);
}

int max(int a, int b){
	if (a > b)
		return a;
	return b;
}

int get_height(node* nd){
	if (nd != NULL)
		return nd->height;
	return -1;
}

// Left Rotation of the AVL Tree
node* rotate_left(node* nd){
	node* root = nd->right;
	nd->right = nd->right->left;
	root->left = nd;

	nd->height = max(get_height(nd->left), get_height(nd->right))+1;
	root->height = max(get_height(root->left), get_height(root->right))+1;

	return root;
}

// Right Rotation of the AVL Tree
node* rotate_right(node* nd){
	node* root = nd->left;
	nd->left = nd->left->right;
	root->right = nd;

	nd->height = max(get_height(nd->left), get_height(nd->right))+1;
	root->height = max(get_height(root->left), get_height(root->right))+1;

	return root;
}

// Inserts a new node with value "val" in the subtree with root "nd", returns the new root
node* insert(node* root, node* nd){

	if (nd == NULL || nd->pr == NULL || PR_PAGEADDR(nd->pr)== NULL){
		printf("ERROR!!! Can't insert null/recylced node to the tree\n");
		exit(1);
	}
	if (nd->height == -1){
		printf("ERROR!!! Node has height -1!\n");		
		return NULL;
	}

	if (root != NULL && root->pr == NULL){
		printf("ERROR!!! Tree root is a recycled node\n");
	}
	// Normal BST insertion
	if (root == NULL){
		return nd;
	} 
	else if (PR_PAGEADDR(root->pr) == PR_PAGEADDR(nd->pr)){

		print_node(nd);
		printf("ERROR!!! Value already exists in the tree: %p\n", PR_PAGEADDR(nd->pr));
		exit(1);
	} 
	else if (PR_PAGEADDR(nd->pr) < PR_PAGEADDR(root->pr)) {
		root->left = insert(root->left, nd);
	} 
	else if (PR_PAGEADDR(nd->pr) > PR_PAGEADDR(root->pr)) {
		root->right = insert(root->right, nd);
	} 

	root->height = max(get_height(root->left), get_height(root->right)) + 1;
	

	// Rebalancing
	int diff = get_height(root->left) - get_height(root->right);

	if (diff < -2 || diff > 2){
		printf("Error! The tree has gone unbalanced! Diff:%d\n", diff);
		inorder(root);
		exit(1);
	}

	if (diff == -2){
		if (PR_PAGEADDR(nd->pr) < PR_PAGEADDR(root->right->pr))
			root->right = rotate_right(root->right);
		return rotate_left(root);
	}

	if (diff == 2){
		if (PR_PAGEADDR(nd->pr) > PR_PAGEADDR(root->left->pr))
			root->left = rotate_left(root->left);
		return rotate_right(root);
	}

	return root;
}


// Finds the smallest element in a subtree. Used for deletion:
node* find_min_node(node* nd){
	if (nd == NULL){
		return NULL;
	} 
	else if (nd->left == NULL)
		return nd;
	else
		return find_min_node(nd->left);
}

// Copies values of node b into node a:
void* copy_node(node* a, node* b){
	a->left = b->left;
	a->right = b->right;
	a->pr = b->pr;
	a->height = b->height;
}

// Deletes node with value "val" in the subtree with root "nd", returns the new root of the subtree.
node* delete(node* root, node* nd, struct heap* h){

	if (nd == NULL || nd->pr == NULL){
		printf("ERROR!!! Can't delete null/recylced node from the tree\n");
		exit(1);
	}
	if (nd->height == -1){
		printf("ERROR!!! delete node has height -1!\n");		
		exit(1);
		return NULL;
	}
	if (root != NULL && root->pr == NULL){
		printf("ERROR!!! Tree root is a recycled node\n");
	}

	if (root == NULL){
		return root;
	}
	else if (PR_PAGEADDR(nd->pr) < PR_PAGEADDR(root->pr)){
		root->left = delete(root->left, nd, h);
	}
	else if (PR_PAGEADDR(nd->pr) > PR_PAGEADDR(root->pr)){
		root->right = delete(root->right, nd, h);
	}
	else {
		node* junk;  // The root holds the node that should be removed

		// Doing the removal based on the children of "root"
		if (root->left == NULL && root->right == NULL){
			freenode(root, h);
			return NULL;
		} else if (root->left == NULL && root->right != NULL){
			junk = root->right;
			copy_node(root, junk);
			freenode(junk, h);
		} else if (root->left != NULL && root->right == NULL){
			junk = root->left;
			copy_node(root, junk);
			freenode(junk, h);
		} else if (root->left != NULL && root->right != NULL){
			junk = find_min_node(root->right);
			root->pr = junk->pr;
			root->right = delete(root->right, junk, h);
		}
	}

	root->height = max(get_height(root->left), get_height(root->right)) + 1;
	
	// Rebalancing
	int diff = get_height(root->left) - get_height(root->right);

	if (diff < -2 || diff > 2){
		printf("Error! The tree has gone unbalanced! Diff:%d\n", diff);
	}

	if (diff == -2){
		if (get_height(root->right->left) > get_height(root->right->right))
			root->right = rotate_right(root->right);
		return rotate_left(root);
	}

	if (diff == 2){
		if (get_height(root->left->left) < get_height(root->left->right))
			root->left = rotate_left(root->left);
		return rotate_right(root);
	}

	return root;
}


// Function for finding the pageref given the ptr_address
node* find_node(node* root, vaddr_t ptr_address){
	if (root == NULL){
		return NULL;
	} 
	else if (PR_PAGEADDR(root->pr) <= ptr_address && ptr_address < PR_PAGEADDR(root->pr) + PAGE_SIZE){
		return root;
	} 
	else if (ptr_address < PR_PAGEADDR(root->pr)){
		return find_node(root->left, ptr_address);
	} 
	else if (ptr_address >= PR_PAGEADDR(root->pr) + PAGE_SIZE){
		return find_node(root->right, ptr_address);
	} 
	else {
		printf("ERROR!!! find in tree: Something has gone wrong!\n");
		return NULL;
	}
}


// Inorder traversal of the tree (For making sure that numbers are sorted)
void inorder(node* root){

	if (root == NULL || root->pr == NULL)
		return;

	if (root->left != NULL)
		inorder(root->left);

	print_node(root);

	if (root->right != NULL)
		inorder(root->right);
}


// Preorder traversal of the tree (For observing the tree structure)
void preorder(int max_height, node* root){
	if (root == NULL)
		return;
	int i;
	for (i=0; i<max_height - root->height; i++){
		printf("=");
	}
	print_node(root);
	preorder(max_height, root->left);
	preorder(max_height, root->right);
}
