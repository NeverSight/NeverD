package fixture;
public interface Provider { <T> void send(Factory<? extends T> factory); }
