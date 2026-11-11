package fixture;
public final class Caller {
    public static void write(Core self,Token token,Object value,int index,long wide) { self.write(token,value,index,wide); }
    public static int count(Core self,Object value) { return self.count(value); }
    public static Object read(Core self,Object value) { return self.read(value); }
}
