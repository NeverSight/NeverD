package fixture;
import api.*;
public final class Child extends other.Owner {
    public Object run(Press scope,long bits,Sink<? super Value> sink) { return this.chooseValue(scope,bits,sink); }
}
