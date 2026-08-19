CREATE DATABASE IF NOT EXISTS cloudstore DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER IF NOT EXISTS 'cloud'@'%' IDENTIFIED BY 'cloud123';
GRANT ALL PRIVILEGES ON cloudstore.* TO 'cloud'@'%';
FLUSH PRIVILEGES;

USE cloudstore;

CREATE TABLE IF NOT EXISTS users (
    id INT PRIMARY KEY AUTO_INCREMENT,
    username VARCHAR(64) NOT NULL UNIQUE,
    password_hash CHAR(64) NOT NULL,
    salt CHAR(32) NOT NULL,
    root_dir VARCHAR(255) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS login_log (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    user_id INT NOT NULL DEFAULT 0,
    ip VARCHAR(64) DEFAULT '',
    success TINYINT NOT NULL DEFAULT 0,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    KEY idx_user (user_id)
) ENGINE=InnoDB;

-- status: ready=用户目录已有独立副本; copying=秒传已可下载，正在后台拷贝到用户目录
CREATE TABLE IF NOT EXISTS file_index (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    user_id INT NOT NULL,
    virtual_path VARCHAR(1024) NOT NULL,
    md5 CHAR(32) DEFAULT '',
    size BIGINT NOT NULL DEFAULT 0,
    is_dir TINYINT NOT NULL DEFAULT 0,
    status VARCHAR(16) NOT NULL DEFAULT 'ready',
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_user_path (user_id, virtual_path(255)),
    KEY idx_md5 (md5)
) ENGINE=InnoDB;
