package other;
import api.*;
public final class Peer {
    public static Object call(Owner receiver,Press scope,long bits,Sink<? super Value> sink) { return receiver.chooseValue(scope,bits,sink); }
}
