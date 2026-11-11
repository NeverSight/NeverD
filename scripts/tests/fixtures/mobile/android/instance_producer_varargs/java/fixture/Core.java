package fixture;
public class Core {
    @SuppressWarnings({"unchecked","rawtypes"})
    public static Object call(Provider receiver,String[] labels,Context context,Producer<?> producer) throws java.io.IOException {
        Trace.hit(1); Object result=receiver.read(context,(Producer)producer,labels);
        Trace.hit(7); Trace.result=result; Trace.after.run(); Trace.hit(8); return result;
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static String checked(Provider receiver,String[] labels,Context context,Producer<?> producer) throws java.io.IOException {
        Trace.hit(1); String result=(String)receiver.read(context,(Producer)producer,labels);
        Trace.hit(7); Trace.result=result; Trace.after.run(); Trace.hit(8); return result;
    }
}
