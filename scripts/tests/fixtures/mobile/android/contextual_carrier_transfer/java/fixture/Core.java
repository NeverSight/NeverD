package fixture;
public class Core {
    @SuppressWarnings({"unchecked","rawtypes"})
    public static Carrier<?> call(Context context,Carrier<?> seed,Class<?> token) throws java.io.IOException {
        Trace.hit(7);
        Carrier<?> result=Provider.transfer(context,(Carrier)seed,(Class)token);
        Trace.hit(8); Trace.result=result; Trace.after.run(); Trace.hit(9);
        return result;
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static Box checked(Context context,Carrier<?> seed,Class<?> token) throws java.io.IOException {
        Trace.hit(7);
        Box result=(Box)Provider.transfer(context,(Carrier)seed,(Class)token);
        Trace.hit(8); Trace.result=result; Trace.after.run(); Trace.hit(9);
        return result;
    }
}
