// A switch whose cases call the function that contains it. Built with -fPIC,
// so the table holds offsets from its own base. The function is static, so
// the assembler resolves its recursive calls without relocations. No case
// combines its calls with an operation that clang folds into a loop.

struct node {
  int kind;
  const struct node *left;
  const struct node *right;
  long value;
};

__attribute__((noinline)) static long evaluate(const struct node *n) {
  switch (n->kind) {
  case 0:
    return n->value;
  case 1:
    return evaluate(n->left) - evaluate(n->right);
  case 2:
    return evaluate(n->left) / (evaluate(n->right) | 1);
  case 3:
    return evaluate(n->left) % (evaluate(n->right) | 1);
  case 4:
    return -evaluate(n->left);
  case 5:
    return evaluate(n->left) << (evaluate(n->right) & 63);
  default:
    return 0;
  }
}

long evaluate_tree(const struct node *n) { return evaluate(n); }
