package fixture;
import java.util.Collection;
public final class ClosedCarrierProducerCaller {
    public static Object call(int operation,Provider receiver,Collection<String> labels,Context context,Producer<?> producer) throws java.io.IOException {
        if(operation==0) return Core.call(receiver,labels,context,producer);
        return Core.checked(receiver,labels,context,producer);
    }
}
