package fixture;
public class TypedConstructorIntegerDefaultCaller {
    public static Base<String> make(int left, Class token, int right) {
        Base<String> result=new Base<String>(left,(Class<String>)token,right);
        Trace.hit(4);
        return result;
    }
    public static Base<String> makeFull(int left, Class token, int padding, int right) {
        Base<String> result=new Base<String>(left,(Class<String>)token,padding,right);
        Trace.hit(4);
        return result;
    }
}
