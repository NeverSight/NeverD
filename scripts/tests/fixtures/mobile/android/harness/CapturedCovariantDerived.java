import fixture.*;
public class CapturedCovariantDerived extends Owner.Worker {
    public int entries;
    public CapturedCovariantDerived(Owner owner) { owner.super(); }
    public Item getItem(int index) { entries++; return super.getItem(index); }
}
