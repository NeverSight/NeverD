package fixture;
public class Host {
    public int calls;
    public Parent make() {
        return new Parent() {
            @Override public Value read(float fraction,Value left,Value right) {
                Host.this.calls++;Trace.children++;Trace.host=Host.this;Trace.hit(1);
                return super.read(fraction,left,right);
            }
        };
    }
}
