package fixture;
public class Core {
    public static Base<String> make(int left, Class token, int right) {
        Base<String> result=new Base<String>(left,(Class<String>)token,right) {};
        Trace.hit(4);
        return result;
    }
}
