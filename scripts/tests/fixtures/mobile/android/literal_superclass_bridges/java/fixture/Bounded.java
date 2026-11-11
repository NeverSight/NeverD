package fixture;
public class Bounded extends Root<Narrow> {
    public void accept(Narrow value, long stamp, Object note) { Trace.hit(4); }
}
