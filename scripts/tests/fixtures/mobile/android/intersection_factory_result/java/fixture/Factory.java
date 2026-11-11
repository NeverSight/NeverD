package fixture;
public abstract class Factory {
    public static Factory reply;
    public Factory() {}
    public static <T extends First & Second> Factory obtain(T input) {
        Trace.hit(1,input,0,0,null);
        int left=input.first();
        int right=input.second();
        Factory result=reply;
        Trace.hit(3,input,left,right,result);
        return result;
    }
}
