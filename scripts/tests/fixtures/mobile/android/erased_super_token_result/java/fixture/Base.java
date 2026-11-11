package fixture;
public class Base {
    public Object[] reply;
    public Base() {}
    @SuppressWarnings("unchecked")
    public <Item> Item[] collect(int left,int right,Class<Item> token) {
        Trace.hit(2,this,token,left,right,reply);
        Object[] result=reply;
        Trace.hit(3,this,token,left,right,result);
        return (Item[])result;
    }
}
