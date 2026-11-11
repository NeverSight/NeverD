package fixture;
public final class ContextualCarrierTransferCaller {
    public static Carrier<?> call(int operation,Context context,Carrier<?> seed,Class<?> token) throws java.io.IOException {
        if(operation==0) return Core.call(context,seed,token);
        return Core.checked(context,seed,token);
    }
}
