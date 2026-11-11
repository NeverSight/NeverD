package fixture;
public interface Api {
    Value fetch();
    Value fetch(int index);
    Value fetch(long stamp, double fraction, Object token);
}
