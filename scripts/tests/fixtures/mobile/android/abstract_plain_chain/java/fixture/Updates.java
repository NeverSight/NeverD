package fixture;

public interface Updates {
    void added(int first, int second);
    void moved(int first, int second);
    void removed(int first, int second);
    void changed(int first, int second, Object payload);
}
