package fixture; public interface Carrier<T,V extends Bound>{ T read(long time,long scale); }
