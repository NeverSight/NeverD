package fixture;
public interface Api {
    <T extends First & Second> void accept(T value);
}
