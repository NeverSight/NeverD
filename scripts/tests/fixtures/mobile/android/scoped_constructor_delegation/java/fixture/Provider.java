package fixture;
public final class Provider {
 public static Object first,second;
 static { Trace.hit(1);first=Trace.seedFirst;second=Trace.seedSecond; }
}
