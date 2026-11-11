package fixture;
@SuppressWarnings("unchecked")
public class Child extends Base<String> {
    public Child(Object first,Object second,Object selector,int index,boolean left,boolean right){super(Provider.wrap(first),Provider.wrap(second),new Adapter(first,selector,index),left,right);}
}
