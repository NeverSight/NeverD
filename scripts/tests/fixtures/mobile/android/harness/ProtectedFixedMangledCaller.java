import api.*;
import fixture.Child;
import other.*;
public final class ProtectedFixedMangledCaller {
    public static Object call(Child receiver,Press scope,long bits,Sink<? super Value> sink) { return receiver.run(scope,bits,sink); }
    public static Object peer(Owner receiver,Press scope,long bits,Sink<? super Value> sink) { return Peer.call(receiver,scope,bits,sink); }
}
