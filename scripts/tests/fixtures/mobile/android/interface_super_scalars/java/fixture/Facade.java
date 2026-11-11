package fixture;
public final class Facade {
    public static Root make(boolean child) { if(child)return new Child();return new Direct(); }
    public static int call(Root receiver,float value,int view) {
        if(view==0)return receiver.measure(value);
        if(view==1)return ((Api)receiver).measure(value);
        if(view==2)return ((Extra)receiver).measure(value);
        if(receiver instanceof Owner)return ((Owner)receiver).measure(value);
        return ((Direct)receiver).measure(value);
    }
}
