package fixture;
public final class Client {
    public static Node make(Object value,Object b,Object c,Object d,Object e,Object f){
        try{return new Node(value,b,c,d,e,f);}catch(IllegalArgumentException failure){Trace.caught=failure;Trace.arguments=new Object[]{value,b,c,d,e,f};Trace.hit(3);return null;}
    }
}
