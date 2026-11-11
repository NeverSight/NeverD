package fixture;
public final class InstanceProducerVarargsCaller {
    public static Object call(int operation,Provider receiver,String[] labels,Context context,Producer<?> producer) throws java.io.IOException {
        if(operation==0) return Core.call(receiver,labels,context,producer);
        return Core.checked(receiver,labels,context,producer);
    }
}
