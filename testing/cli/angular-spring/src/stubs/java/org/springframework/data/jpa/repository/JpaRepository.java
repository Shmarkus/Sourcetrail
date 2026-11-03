package org.springframework.data.jpa.repository;

import java.util.List;
import java.util.Optional;

public interface JpaRepository<T, ID> {
    List<T> findAll();

    Optional<T> findById(ID id);

    <S extends T> S save(S entity);
}
