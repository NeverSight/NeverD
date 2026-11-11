package fixture;
public class Core {
    public Value value;
    public Child receiver;
    public Api contract;
    public void runVirtual() {
        Value selected = value;
        Trace.hit(1);
        receiver.accept(selected);
    }
    public void runInterface() {
        Value selected = value;
        Trace.hit(1);
        contract.accept(selected);
    }
}
