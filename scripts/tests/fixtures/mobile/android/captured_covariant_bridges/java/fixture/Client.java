package fixture;
public class Client {
    public static Object read(Base self,int index) { return self.getItem(index); }
    public static Item narrow(Owner.Worker self,int index) { return self.getItem(index); }
    public static Object privateRead(Owner owner,int index) { return owner.createPrivate().getItem(index); }
}
