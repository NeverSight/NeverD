package fixture;
final class Owner extends Host.Base implements SideA, Api, SideB, SideC {
    @Override public void signal() { Api.super.signal(); }
}
