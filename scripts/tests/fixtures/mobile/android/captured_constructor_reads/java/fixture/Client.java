package fixture;
public class Client {
    public static Base make(Owner owner, Object payload, boolean wide) {
        if (wide) return owner.createWide(payload);
        return owner.create(payload);
    }
}
