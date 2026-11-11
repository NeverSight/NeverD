package fixture;
public final class Node {
    public final Object value;
    public final Object[] arguments;
    public Node(Object value,Object b,Object c,Object d,Object e,Object f){Trace.partial=this;Trace.hit(1);this.value=value;this.arguments=new Object[]{value,b,c,d,e,f};Trace.hit(2);}
}
