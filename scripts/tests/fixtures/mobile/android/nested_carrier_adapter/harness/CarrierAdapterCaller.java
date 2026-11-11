import fixture.*;
public final class CarrierAdapterCaller {
 public static Object call(int operation,Pair input){return operation==0?Client.whole(input):operation==1?Client.first(input):Client.tail(input);}
}
