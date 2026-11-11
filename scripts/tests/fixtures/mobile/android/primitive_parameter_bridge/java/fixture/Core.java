package fixture;
public final class Core {
    private Core() {}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static float get(Object receiver,Object value) {
        Trace.hit(0,value,0.0f);
        float result=((Base)receiver).get(value);
        Trace.hit(4,value,result);
        return result;
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void put(Object receiver,Object value,float number) {
        Trace.hit(0,value,0.0f);
        ((Base)receiver).put(value,number);
        Trace.hit(4,value,number);
    }
}
