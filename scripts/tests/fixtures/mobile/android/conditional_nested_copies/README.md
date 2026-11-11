# Conditional nested constructor copies

First-party Java sources exercise a nested constructor selected inside an outer
constructor argument program. D8 must retain reference and two wide argument
copies before the nested range invocation. The independent frozen caller checks
both long and double raw bits, both branches, initialization order, partial
constructor publication, exact body failure identity and repeated initialization
failures. The original classfiles and SDK declarations stay immutable.
