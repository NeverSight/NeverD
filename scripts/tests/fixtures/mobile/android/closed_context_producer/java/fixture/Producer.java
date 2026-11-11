package fixture;
public interface Producer<E> extends java.io.Serializable { E produce(Ticket ticket); }
