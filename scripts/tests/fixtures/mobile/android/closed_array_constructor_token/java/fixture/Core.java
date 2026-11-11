package fixture;
public class Core {
    public static Base<float[]> make(int left, Class token, int right) {
        Base<float[]> result=new Base<float[]>(left,(Class<float[]>)token,right) {};
        Trace.hit(4);
        return result;
    }
}
