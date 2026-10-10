// NeverD's parser-only explicit resource management extension. Included inside
// hermes::parser::detail in the private, pinned JSParserImpl.cpp copy.
// Grammar reference: ECMA-262 14.3.1, 14.7.4 and 14.7.5 (2026-10-11).
// Keep the ESTree declaration kind: no lowering, target execution or rewriting.

bool JSParserImpl::checkResourceDeclaration(bool inFor) {
  if (!check(TokenKind::identifier))
    return false;
  const bool async = paramAwait_ && tok_->inputStr() == "await";
  if (!async && tok_->inputStr() != "using")
    return false;

  // Both keywords are contextual and must be unescaped. SavePoint also owns
  // token/comment storage and the preceding-newline flag when backing out.
  JSLexer::SavePoint saved(&lexer_);
  bool result = true;
  if (async) {
    advance();
    result = !lexer_.isNewLineBeforeCurrentToken() &&
             check(TokenKind::identifier) && tok_->inputStr() == "using";
  }
  if (result) {
    advance();
    result = !lexer_.isNewLineBeforeCurrentToken() &&
             check(TokenKind::identifier, TokenKind::rw_yield);
    // `for (using of values)` assigns to an existing `using` binding.
    // `for (using of of values)` cannot declare an `of` binding.
    if (inFor && !async && tok_->inputStr() == "of")
      result = false;
  }
  saved.restore();
  return result;
}

Optional<ESTree::VariableDeclarationNode *>
JSParserImpl::parseResourceDeclaration(Param param, bool inFor) {
  const SMLoc start = tok_->getStartLoc();
  const bool async = tok_->inputStr() == "await";
  if (async)
    advance();
  advance(); // using

  ESTree::NodeList declarations;
  llvh::SmallDenseSet<UniqueString *, 8> names;
  do {
    // No array/object binding patterns, including after a comma.
    const SMLoc bindingStart = tok_->getStartLoc();
    auto binding = parseBindingIdentifier(Param{});
    if (!binding) {
      error(bindingStart, "resource declaration requires a binding identifier");
      return None;
    }
    if (!names.insert((*binding)->_name).second)
      error(bindingStart, "duplicate resource binding");
    ESTree::Node *init = nullptr;
    if (checkAndEat(TokenKind::equal)) {
      auto value = parseAssignmentExpression(
          param, AllowTypedArrowFunction::Yes, CoverTypedParameters::No);
      if (!value)
        return None;
      init = *value;
    }
    declarations.push_back(*setLocation(
        bindingStart, getPrevTokenEndLoc(),
        new (context_) ESTree::VariableDeclaratorNode(init, *binding)));
  } while (checkAndEat(TokenKind::comma));

  // The only uninitialized form is a single for-of binding. The loop parser
  // and semantic validator own the count and initialized-for-of checks.
  if (inFor && check(TokenKind::rw_in)) {
    error(start, "resource declaration is not allowed in for-in");
    return None;
  }
  if (!inFor || !check(ofIdent_)) {
    for (const auto &node : declarations)
      if (!cast<ESTree::VariableDeclaratorNode>(&node)->_init)
        error(node.getSourceRange(), "missing resource initializer");
  }
  if (!inFor && !eatSemi())
    return None;
  return setLocation(start, getPrevTokenEndLoc(),
                     new (context_) ESTree::VariableDeclarationNode(
                         lexer_.getIdentifier(async ? "await using" : "using"),
                         std::move(declarations)));
}
