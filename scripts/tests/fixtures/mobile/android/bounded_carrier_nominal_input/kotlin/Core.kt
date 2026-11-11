package fixture
class Namespace { open class Node }
abstract class Element<N:Namespace.Node> { abstract fun update(node:N) }
@Suppress("UNCHECKED_CAST")
private fun <T:Namespace.Node> updateUnsafe(element:Element<T>,node:Namespace.Node) {
    element.update(node as T)
}
fun send(element:Element<Namespace.Node>,node:Namespace.Node) { updateUnsafe(element,node) }
