package fixture;
import api.Bound;
public class Child extends parent.Base {
    public static Bound apply(Child receiver,Bound value) {
        return receiver.choose(value);
    }
    public static Bound copied(Child receiver,Bound value) {
        Child alias=receiver;
        return alias.choose(value);
    }
}
