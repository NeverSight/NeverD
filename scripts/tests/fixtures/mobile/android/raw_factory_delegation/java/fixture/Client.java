package fixture;
public final class Client {
 public static Core call(){Core result=new Core();Trace.after=result;Trace.hit(7);return result;}
}
