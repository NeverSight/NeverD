package fixture;
public class Core {
    @SuppressWarnings({"unchecked","rawtypes"})
    public static Object call(Provider receiver,Closed<Context,Ticket> setup,Context context,Producer<?> producer) throws java.io.IOException {
        Trace.hit(1); Object result=Provider.read(receiver,setup,context,(Producer)producer);
        Trace.hit(7); Trace.result=result; Trace.after.run(); Trace.hit(8); return result;
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static String checked(Provider receiver,Closed<Context,Ticket> setup,Context context,Producer<?> producer) throws java.io.IOException {
        Trace.hit(1); String result=(String)Provider.read(receiver,setup,context,(Producer)producer);
        Trace.hit(7); Trace.result=result; Trace.after.run(); Trace.hit(8); return result;
    }
}
