package fixture;
public class Node<E> implements Tagged<Node<E>> {
    public E value;
    public Context previous;
    public Node(E value,Context previous) {
        Trace.constructors++;
        Trace.hit(1);
        this.value=value;
        this.previous=previous;
        Trace.constructed=this;
        Trace.action.run();
        Trace.hit(2);
    }
    public static <T> Node<T> make(T value) {return make(value,null);}
    @SuppressWarnings("unchecked") public static <T> Node<T> make(T value,Context previous) {
        Node<T> created=new Node<T>(value,previous);
        Trace.created=created;
        Trace.hit(3);
        return (Node<T>)Trace.result;
    }
}
