package fixture;
public final class ClosedContextProducerCaller {
    public static Object call(int operation,Provider receiver,Closed<Context,Ticket> setup,Context context,Producer<?> producer) throws java.io.IOException {
        if(operation==0) return Core.call(receiver,setup,context,producer);
        return Core.checked(receiver,setup,context,producer);
    }
}
